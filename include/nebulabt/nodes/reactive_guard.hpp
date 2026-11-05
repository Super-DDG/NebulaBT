#pragma once
/// ReactiveGuard:运行期安全守护(M5),装饰器。
///
///   <ReactiveGuard while_true="{safe_to_proceed}" period_ms="100">
///     <Navigate/>          <!-- 长时运行的动作 -->
///   </ReactiveGuard>
///
/// 语义(与失败自动捕获同一哲学:保险由框架包裹,不靠作者自觉):
/// - 进入子树前与子树 RUNNING 期间,每 period_ms 毫秒检查一次
///   `while_true` 引用的黑板 bool 键
/// - 键为 false(或键不存在/类型不符,**fail-closed**)→ halt 子树 +
///   记 GUARD_VIOLATED 失败 → 守护节点 FAILURE(父层走恢复分支)
///   (无 schema 提交时 bool slots 会以字符串 "true"/"false" 落黑板,
///   守护兼容该形态;键完全缺失仍按违反处理)
/// - 检查由调度器定时器驱动(事件驱动的唤醒源之一,非业务轮询):
///   定时器到期 → 唤醒任务 → 整树 reactive 重 tick → 守护节点重估条件。
///   RUNNING 中的 StatefulActionNode 重 tick 返回缓存状态,不会重跑
/// - 子树终态后停止巡检
///
/// 适用:安全区检查、e-stop、电量下限、依赖健康——"子树只能在条件
/// 成立时运行"这一机器人刚需的声明式表达。

#include "nebulabt/core/scheduler.hpp"
#include <behaviortree_cpp/decorator_node.h>
#include <functional>
#include <string>

namespace nebulabt {

/// 宿主接口:由 EngineCore 实现,记录守护违规(失败数据 + journal)
class IGuardHost {
public:
  virtual ~IGuardHost() = default;
  virtual void guardViolated(const std::string& task_id,
                             const std::string& node_name,
                             const std::string& guard_key) = 0;
};

class ReactiveGuardNode : public BT::DecoratorNode {
public:
  ReactiveGuardNode(const std::string& name, const BT::NodeConfig& config,
                    Scheduler* scheduler,
                    std::function<void(const std::string& task_id)> wake,
                    IGuardHost* host);

  static BT::PortsList providedPorts();

  BT::NodeStatus tick() override;
  void halt() override;

private:
  /// 读 while_true 键;缺失/类型不符按违反处理(fail-closed)
  bool guardHolds() const;
  std::string taskIdFromBlackboard() const;
  /// 子树 RUNNING 期间武装下一次巡检(已武装则不重复)
  void armInspection();

  Scheduler* scheduler_;
  std::function<void(const std::string&)> wake_;
  IGuardHost* host_;
  int period_ms_ = 100;
  std::string guard_key_;
  bool inspection_armed_ = false;
};

} // namespace nebulabt
