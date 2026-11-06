#include "nebulabt/nodes/checkpoint.hpp"

#include "nebulabt/core/any_json.hpp"

#include <behaviortree_cpp/blackboard.h>
#include <set>
#include <vector>

namespace nebulabt {

CheckpointNode::CheckpointNode(const std::string& name,
                               const BT::NodeConfig& config,
                               ICheckpointHost* host)
  : BT::DecoratorNode(name, config), host_(host) {}

BT::PortsList CheckpointNode::providedPorts() {
  return {
    BT::InputPort<std::string>("id", "检查点标识;同树内唯一,恢复时按它匹配"),
  };
}

std::string CheckpointNode::taskIdFromBlackboard() const {
  std::string taskId;
  if (auto bb = config().blackboard) {
    (void)bb->get("task_id", taskId);
  }
  return taskId;
}

// 快照:枚举黑板全部键(排除框架键),按 Any::type() 精确分发。
// 注意:不能用 bb->get<T>(key, out) 逐类型试探——BT.CPP 对类型不匹配的
// 键会抛 Any::convert 异常,而不是返回 false。
nlohmann::json CheckpointNode::captureSnapshot() const {
  auto bb = config().blackboard;
  nlohmann::json snapshot = nlohmann::json::object();
  if (!bb) {
    return snapshot;
  }
  static const std::set<std::string> kFrameworkKeys = {"task_id", "task_type"};
  for (const auto keyView : bb->getKeys()) {
    const std::string key(keyView);
    if (kFrameworkKeys.count(key)) {
      continue;
    }
    auto entry = bb->getAnyLocked(key);
    if (!entry) {
      continue;
    }
    const nlohmann::json v = nebulabt::anyToJson(entry);
    if (!v.is_null()) {
      snapshot[key] = v;
    }
    // 其它类型 v0 跳过(见头文件已知限制)
  }
  return snapshot;
}

void CheckpointNode::restoreSnapshot(const nlohmann::json& snapshot) {
  auto bb = config().blackboard;
  if (!bb || !snapshot.is_object()) {
    return;
  }
  for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
    const auto& v = it.value();
    if (v.is_string()) {
      bb->set(it.key(), v.get<std::string>());
    } else if (v.is_number_integer()) {
      bb->set(it.key(), v.get<int64_t>());
    } else if (v.is_number_float()) {
      bb->set(it.key(), v.get<double>());
    } else if (v.is_boolean()) {
      bb->set(it.key(), v.get<bool>());
    } else if (v.is_array()) {
      std::vector<std::string> vec;
      for (const auto& e : v) {
        vec.push_back(e.is_string() ? e.get<std::string>() : e.dump());
      }
      bb->set(it.key(), vec);
    }
  }
}

BT::NodeStatus CheckpointNode::tick() {
  if (status() == BT::NodeStatus::IDLE) {
    cp_id_ = getInput<std::string>("id").value_or("");
    if (cp_id_.empty()) {
      throw BT::RuntimeError("Checkpoint: 'id' port is required");
    }
    task_id_ = taskIdFromBlackboard();
    resumed_ = false;
    // 恢复:该检查点上一轮已完成 -> 跳过子节点,恢复黑板
    nlohmann::json snap;
    if (host_ && host_->checkpointTryResume(task_id_, cp_id_, &snap)) {
      snapshot_ = std::move(snap);
      restoreSnapshot(snapshot_);
      resumed_ = true;
      resetChild();
      return BT::NodeStatus::SUCCESS;
    }
  }

  if (resumed_) {
    return BT::NodeStatus::SUCCESS;  // 父节点 RUNNING 期间保持已跳过状态
  }

  const BT::NodeStatus childStatus = child()->executeTick();
  if (childStatus == BT::NodeStatus::SUCCESS) {
    snapshot_ = captureSnapshot();
    if (host_) {
      host_->checkpointRecord(task_id_, cp_id_, snapshot_);
    }
    resetChild();
  }
  return childStatus;
}

void CheckpointNode::halt() {
  resumed_ = false;
  DecoratorNode::halt();
}

} // namespace nebulabt
