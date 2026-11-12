#include "nebulabt/nodes/for_each.hpp"

#include "nebulabt/core/any_json.hpp"
#include <behaviortree_cpp/blackboard.h>
#include <typeindex>

namespace nebulabt {

ForEachNode::ForEachNode(const std::string& name, const BT::NodeConfig& config)
  : BT::ControlNode(name, config) {}

BT::PortsList ForEachNode::providedPorts() {
  return {
    // 无类型端口:绑定的黑板键可能是 vector<string> 或 json 数组
    BT::InputPort("array",
        "要遍历的集合:vector<string>、json 数组,或 ';' 分隔字符串"),
    BT::OutputPort<std::string>("element", "当前元素(字符串形式写入黑板)"),
    BT::OutputPort<nlohmann::json>("element_json",
        "当前元素(真实 json;仅当集合为 json 数组且本端口被绑定时写入)"),
    BT::InputPort<bool>("continue_on_failure", false,
        "子节点失败时跳过该元素继续,而非整体失败"),
  };
}

BT::NodeStatus ForEachNode::tick() {
  if (children().size() != 1) {
    throw BT::RuntimeError("ForEach: requires exactly one child");
  }
  if (status() == BT::NodeStatus::IDLE) {
    items_ = resolveArray();
    index_ = 0;
  }

  const bool continueOnFailure = getInput<bool>("continue_on_failure").value_or(false);

  while (index_ < items_.size()) {
    auto setOk = setOutput("element", items_[index_]);
    if (!setOk) {
      throw BT::RuntimeError("ForEach: 'element' port must be remapped: ",
                             setOk.error());
    }
    // json 数组来源:记录当前元素供 element_json 写出
    current_json_ = (!json_array_.is_null() &&
                     index_ < json_array_.size())
                        ? json_array_[index_] : nlohmann::json(nullptr);
    if (!current_json_.is_null()) {
      // 仅当 element_json 被绑定时写(未绑定的 Output 写入会报错)
      if (config().output_ports.count("element_json")) {
        setOutput("element_json", current_json_);
      }
    }
    const BT::NodeStatus childStatus = children()[0]->executeTick();
    if (childStatus == BT::NodeStatus::RUNNING) {
      return BT::NodeStatus::RUNNING;
    }
    if (childStatus == BT::NodeStatus::FAILURE && !continueOnFailure) {
      haltChild(0);
      return BT::NodeStatus::FAILURE;
    }
    haltChild(0); // 重置子节点,进入下一个元素
    ++index_;
  }
  return BT::NodeStatus::SUCCESS;
}

void ForEachNode::halt() {
  index_ = 0;
  current_json_ = nullptr;
  ControlNode::halt();
}

std::vector<std::string> ForEachNode::resolveArray() {
  std::vector<std::string> out;
  auto bb = config().blackboard;
  if (!bb) {
    throw BT::RuntimeError("ForEach: no blackboard");
  }
  // array 端口的原始绑定:{key} → 黑板键名;字面量 → ';' 分隔字符串
  std::string keyOrLiteral;
  auto it = config().input_ports.find("array");
  if (it != config().input_ports.end()) {
    keyOrLiteral = it->second;
  }
  // 1) {key} 引用:按真实类型分发
  if (keyOrLiteral.size() >= 3 && keyOrLiteral.front() == '{' &&
      keyOrLiteral.back() == '}') {
    const std::string key = keyOrLiteral.substr(1, keyOrLiteral.size() - 2);
    auto entry = bb->getAnyLocked(key);
    if (!entry) {
      throw BT::RuntimeError("ForEach: blackboard key '" + key + "' not found");
    }
    const std::type_index t = entry->type();
    if (t == typeid(std::vector<std::string>)) {
      return entry->cast<std::vector<std::string>>();
    }
    if (t == typeid(nlohmann::json)) {
      const nlohmann::json arr = entry->cast<nlohmann::json>();
      if (!arr.is_array()) {
        throw BT::RuntimeError("ForEach: key '" + key + "' is not an array");
      }
      json_array_ = arr;
      for (const auto& e : arr) {
        out.push_back(e.is_string() ? e.get<std::string>() : e.dump());
      }
      return out;
    }
    throw BT::RuntimeError("ForEach: key '" + key + "' has unsupported type");
  }

  // 2) 字面量:';' 分隔字符串
  std::string cur;
  for (const char ch : keyOrLiteral) {
    if (ch == ';') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  json_array_ = nullptr;
  return out;
}

} // namespace nebulabt