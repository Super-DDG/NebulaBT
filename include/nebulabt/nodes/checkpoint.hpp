#pragma once
/// Checkpoint:断点恢复原语(M2),装饰器。
///
///   <Sequence>
///     <SearchStep/>                      <!-- 有副作用:导航/检测/抓取 -->
///     <Checkpoint id="search_done"/>
///     <GraspStep/>
///     <Checkpoint id="grasp_done"/>
///     <HandoverStep/>                    <!-- 可能失败的后续段 -->
///   </Sequence>
///
/// 语义:
/// - 子节点 SUCCESS:捕获**全量黑板快照**(排除框架键)记入 journal 并通知宿主
/// - 任务以 resume_journal 重新提交时:已完成的 Checkpoint **跳过子节点**,
///   直接恢复黑板快照并返回 SUCCESS——上一轮已提交的副作用不重复执行
///   (前提:业务节点幂等,见 DESIGN 4.7)
///
/// 恢复流程(对应"kill -9 后重跑同一棵树"):
///   1. 拿到上一轮 journal(经 sink 落盘,或 TaskHandle::journal())
///   2. TaskSpec.resume_journal = journal
///   3. 提交同一棵树:执行到已完成 Checkpoint 处短路跳过,未完成段正常执行
///
/// 快照支持类型:string / int / int64 / double / bool / vector<string>;
/// 其它类型跳过(v0 已知限制)。

#include <behaviortree_cpp/decorator_node.h>
#include <behaviortree_cpp/contrib/json.hpp>
#include <string>

namespace nebulabt {

/// 宿主接口:由 Engine 实现,Checkpoint 节点经它读写任务的检查点历史
class ICheckpointHost {
public:
  virtual ~ICheckpointHost() = default;
  /// 恢复:该任务的该检查点是否已完成;是则返回黑板快照(命中即记
  /// checkpoint_restored 事件)
  virtual bool checkpointTryResume(const std::string& task_id,
                                   const std::string& cp_id,
                                   nlohmann::json* out_snapshot) = 0;
  /// 检查点完成:记录快照 + journal 事件
  virtual void checkpointRecord(const std::string& task_id,
                                const std::string& cp_id,
                                const nlohmann::json& snapshot) = 0;
};

class CheckpointNode : public BT::DecoratorNode {
public:
  CheckpointNode(const std::string& name, const BT::NodeConfig& config,
                 ICheckpointHost* host);

  static BT::PortsList providedPorts();

  BT::NodeStatus tick() override;
  void halt() override;

private:
  std::string taskIdFromBlackboard() const;
  nlohmann::json captureSnapshot() const;
  void restoreSnapshot(const nlohmann::json& snapshot);

  ICheckpointHost* host_;
  std::string cp_id_;
  std::string task_id_;
  nlohmann::json snapshot_ = nlohmann::json::object();
  bool resumed_ = false;
};

} // namespace nebulabt
