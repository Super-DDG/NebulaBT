#pragma once
/// Compensable:Saga 补偿原语(M5),控制节点(双子:主工作 + 补偿)。
///
///   <Sequence>
///     <Compensable id="reserve" compensation_id="release">
///       <ReserveStock/>          <!-- 有副作用的主工作 -->
///       <ReleaseStock/>          <!-- 补偿子树(正常运行永不执行) -->
///     </Compensable>
///     <Compensable id="charge" compensation_id="refund">
///       <Charge/>
///       <Refund/>
///     </Compensable>
///     <Ship/>                    <!-- 这里失败 -->
///   </Sequence>
///
/// 语义:
/// - 主工作 SUCCESS → 本节点"已武装"(sagaPush,记 journal
///   compensation_armed);补偿子树保持 IDLE,正常运行永不执行
/// - 任务终态 FAILED → 引擎按**武装逆序**自动执行已武装节点的补偿子树
///   —— 扣了 A、B 失败,忘补偿 A 这类事故在结构上不可能发生
/// - 补偿同步执行(调度线程,事件驱动模型),单条补偿最多 tick
///   kMaxCompensationTicks 次(防补偿内部死循环);补偿失败记
///   COMPENSATION_FAILED,不中断其余补偿(逆序继续)
/// - SUCCEEDED / CANCELED 不触发补偿(取消是否补偿属业务语义,由
///   业务在 halt() 里自行决定)
///
/// journal 事件:compensation_armed / compensation_started /
/// compensation_done {id, status}

#include <behaviortree_cpp/control_node.h>
#include <string>
#include <vector>

namespace nebulabt {

class CompensableNode;

/// 宿主接口:由 EngineCore 实现,管理任务的已武装补偿栈
class ISagaHost {
public:
  virtual ~ISagaHost() = default;

  /// 主工作完成:该节点进入任务的补偿栈(按执行顺序)
  virtual void sagaPush(const std::string& task_id, CompensableNode* node,
                        const std::string& saga_id) = 0;

  /// 任务失败:取走全部已武装补偿(返回按武装顺序,引擎逆序执行)
  virtual std::vector<std::pair<CompensableNode*, std::string>> sagaPopAll(
      const std::string& task_id) = 0;

  /// 任务非失败终态:丢弃补偿栈
  virtual void sagaClear(const std::string& task_id) = 0;
};

class CompensableNode : public BT::ControlNode {
public:
  static constexpr int kMaxCompensationTicks = 1000;

  CompensableNode(const std::string& name, const BT::NodeConfig& config,
                  ISagaHost* host);

  static BT::PortsList providedPorts();

  BT::NodeStatus tick() override;

  /// 任务失败后由引擎调用:同步执行补偿子树(子节点[1]),返回终态
  BT::NodeStatus executeCompensation();

  const std::string& sagaId() const { return saga_id_; }

private:
  ISagaHost* host_;
  std::string saga_id_;
  std::string task_id_;
};

} // namespace nebulabt
