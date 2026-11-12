#include "tree_check.hpp"

#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/control_node.h>
#include <behaviortree_cpp/decorator_node.h>
#include <behaviortree_cpp/tree_node.h>
#include <set>

namespace nebulabt {
namespace detail {

namespace {

struct KeyRef {
  std::string key;
  std::string node;
  std::string port;
  bool is_write;
};

bool parseKeyRef(const std::string& remap, std::string* keyOut) {
  if (remap.size() < 3 || remap.front() != '{' || remap.back() != '}') {
    return false;
  }
  std::string key = remap.substr(1, remap.size() - 2);
  // 去空白
  const auto first = key.find_first_not_of(" \t");
  const auto last = key.find_last_not_of(" \t");
  if (first == std::string::npos) {
    return false;
  }
  *keyOut = key.substr(first, last - first + 1);
  return !keyOut->empty();
}

void collectKeyRefs(const BT::TreeNode* node, std::vector<KeyRef>& refs) {
  for (const auto& [port, remap] : node->config().input_ports) {
    std::string key;
    if (parseKeyRef(remap, &key)) {
      refs.push_back(KeyRef{key, node->name(), port, false});
    }
  }
  for (const auto& [port, remap] : node->config().output_ports) {
    std::string key;
    if (parseKeyRef(remap, &key)) {
      refs.push_back(KeyRef{key, node->name(), port, true});
    }
  }
  if (auto ctrl = dynamic_cast<const BT::ControlNode*>(node)) {
    for (const BT::TreeNode* child : ctrl->children()) {
      collectKeyRefs(child, refs);
    }
  } else if (auto dec = dynamic_cast<const BT::DecoratorNode*>(node)) {
    if (dec->child()) {
      collectKeyRefs(dec->child(), refs);
    }
  }
}

} // namespace

void validateTreeKeys(const BT::Tree& tree,
                      const std::map<std::string, SlotDef>& defs,
                      std::vector<std::string>& errors) {
  std::vector<KeyRef> refs;
  collectKeyRefs(tree.rootNode(), refs);

  std::set<std::string> produced{"task_id", "task_type"};  // 框架注入
  for (const auto& [key, def] : defs) {
    produced.insert(key);
  }
  for (const auto& ref : refs) {
    if (ref.is_write) {
      produced.insert(ref.key);
    }
  }
  for (const auto& ref : refs) {
    if (!ref.is_write && !produced.count(ref.key)) {
      errors.push_back("node '" + ref.node + "' port '" + ref.port +
                       "' reads undeclared blackboard key '" + ref.key +
                       "' (not in schema and never written by the tree)");
    }
  }
}

void injectSlots(BT::Tree& tree, const TaskSpec& spec,
                 const std::map<std::string, SlotDef>& defs) {
  auto bb = tree.rootBlackboard();
  bb->set("task_id", spec.task_id);
  bb->set("task_type", spec.task_type);
  for (auto it = spec.slots.begin(); it != spec.slots.end(); ++it) {
    const auto& v = it.value();
    // schema 声明的标量类型按真实类型注入(类型已在 L1 校验通过)
    auto defIt = defs.find(it.key());
    if (defIt != defs.end()) {
      const auto& type = defIt->second.type;
      if (type == "int" && v.is_number_integer()) {
        bb->set(it.key(), v.get<int>());
        continue;
      }
      if (type == "double" && v.is_number()) {
        bb->set(it.key(), v.get<double>());
        continue;
      }
      if (type == "bool" && v.is_boolean()) {
        bb->set(it.key(), v.get<bool>());
        continue;
      }
    }
    // schema 声明的 json 类型:黑板存真实 json(结构化黑板)
    if (defIt != defs.end() && (defIt->second.type == "json" ||
                                defIt->second.type == "json_array")) {
      bb->set(it.key(), v);
      continue;
    }
    if (v.is_string()) {
      bb->set(it.key(), v.get<std::string>());
    } else if (v.is_boolean()) {
      // 无 schema 时 bool 按真实类型注入(旧规则 dump 成 "true" 字符串,
      // 是 ReactiveGuard/类型钉死一类坑的根源)
      bb->set(it.key(), v.get<bool>());
    } else if (v.is_array()) {
      bool allStrings = true;
      for (const auto& e : v) {
        if (!e.is_string()) {
          allStrings = false;
          break;
        }
      }
      if (allStrings) {
        // 兼容路径:string 数组 → vector<string>(ForEach/业务直接可用)
        std::vector<std::string> vec;
        vec.reserve(v.size());
        for (const auto& e : v) {
          vec.push_back(e.get<std::string>());
        }
        bb->set(it.key(), vec);
      } else {
        // 结构化数组(含对象/混合类型)→ 真实 json 数组
        bb->set(it.key(), v);
      }
    } else {
      bb->set(it.key(), v.dump());
    }
  }
}

void walkReactive(const BT::TreeNode* node, bool underReactive,
                  std::vector<std::string>& warnings) {
  const std::string reg = node->registrationName();
  const bool isReactive = reg == "ReactiveSequence" || reg == "ReactiveFallback";
  if (underReactive && (reg == "Compensable" || reg == "Checkpoint")) {
    warnings.push_back(std::string("node '") + node->name() + "' (" + reg +
                       ") sits under a reactive parent: completed nodes are "
                       "halt-reset and re-run on every wake, so its side "
                       "effects repeat; place it under a memory Sequence "
                       "(or make it idempotent)");
  }
  const bool childReactive = underReactive || isReactive;
  if (auto ctrl = dynamic_cast<const BT::ControlNode*>(node)) {
    for (const BT::TreeNode* child : ctrl->children()) {
      walkReactive(child, childReactive, warnings);
    }
  } else if (auto dec = dynamic_cast<const BT::DecoratorNode*>(node)) {
    if (dec->child()) {
      walkReactive(dec->child(), childReactive, warnings);
    }
  }
}

std::vector<std::string> warnSideEffectsUnderReactive(const BT::Tree& tree) {
  std::vector<std::string> warnings;
  if (tree.rootNode()) {
    walkReactive(tree.rootNode(), false, warnings);
  }
  return warnings;
}

} // namespace detail
} // namespace nebulabt
