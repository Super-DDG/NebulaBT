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
    if (v.is_string()) {
      bb->set(it.key(), v.get<std::string>());
    } else if (v.is_array()) {
      std::vector<std::string> vec;
      vec.reserve(v.size());
      for (const auto& e : v) {
        vec.push_back(e.is_string() ? e.get<std::string>() : e.dump());
      }
      bb->set(it.key(), vec);
    } else {
      bb->set(it.key(), v.dump());
    }
  }
}

} // namespace detail
} // namespace nebulabt
