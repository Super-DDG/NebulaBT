#pragma once
/// NebulaBT 能力层 SDK:业务动作基类与执行上下文。
/// 设计决策见 docs/adr/ADR-002-callback-continuation-node-api.md
///
/// 写业务,不写胶水:
/// - 同步动作:start 里直接 return SUCCESS / FAILURE
/// - 异步动作:start 里发起调用、保存 done,返回 RUNNING;
///   完成时在任意线程调用 done,调度器被唤醒、树继续推进
/// - halt:取消/树被 halt 时回调,应当取消在途工作
/// 节点作者全程不感知 tick、线程与轮询。

#include "nebulabt/core/any_json.hpp"
#include "nebulabt/core/failure.hpp"
#include <behaviortree_cpp/action_node.h>
#include <behaviortree_cpp/bt_factory.h>
#include <functional>
#include <memory>
#include <string>

namespace nebulabt {

namespace detail {
class BridgeNode;
}

/// 完成续传回调。业务代码完成后在任意线程调用:
///   done(BT::NodeStatus::SUCCESS, {{"price", 3.14}});
/// result 中的键会按端口声明写回该节点的 Output 端口。
///
/// 保留键(M8):done 在任意线程执行时无法安全访问 ctx;result 可携带
///   "__fail_code" / "__fail_message"
/// 由桥在调度线程内代记结构化失败——异步包装器的校验逻辑只依赖捕获
/// 副本,零 ctx 访问,天然线程安全。
using Done = std::function<void(BT::NodeStatus status, nlohmann::json result)>;

/// 节点执行上下文:类型化端口读写 + 任务元信息 + 结构化失败记录。
class NebulaCtx {
public:
  /// 读 Input 端口;端口缺失/类型不符时返回 fallback
  template <typename T>
  T in(const std::string& port, const T& fallback = T()) const {
    auto res = node_->getInput<T>(port);
    if (!res) {
      return fallback;
    }
    return *res;
  }

  /// 写 Output 端口(要求该端口已在 providedPorts 声明)
  template <typename T>
  void out(const std::string& port, const T& value) {
    auto res = node_->setOutput(port, value);
    if (!res) {
      fail("PORT_OUTPUT_ERROR", "out(\"" + port + "\"): " + res.error());
    }
  }

  /// 记录一条结构化失败(建议在返回 FAILURE 前调用)
  void fail(std::string code, std::string message);

  /// 输入端口快照(激活时刻):{key} 引用解析为黑板值(Any 类型分发),
  /// 字面量保持 string。用途:mock 调用录制、业务日志、调试。
  nlohmann::json inputsJson() const { return inputs_; }

  /// 端口绑定的黑板键名:port="slot_name" 形式;字面量绑定返回空串。
  /// 供"按名字检查任意黑板键"的条件类节点使用(如 CheckSlot)
  std::string inRef(const std::string& port) const;

  /// 按名字读黑板键(Any 类型分发;键缺失返回 null)。
  /// 与 inRef 组合支持动态键访问;常规数据流仍应走端口
  nlohmann::json peek(const std::string& blackboardKey) const;

  const std::string& taskId() const { return task_id_; }
  const std::string& taskType() const { return task_type_; }

private:
  friend class detail::BridgeNode;
  NebulaCtx(BT::TreeNode& node, std::shared_ptr<FailureLog> failures,
            std::string taskId, std::string taskType,
            std::shared_ptr<FailureCodeTable> codes);

  BT::TreeNode* node_;
  std::shared_ptr<FailureLog> failures_;
  std::shared_ptr<FailureCodeTable> codes_;
  std::string task_id_;
  std::string task_type_;
  nlohmann::json inputs_ = nlohmann::json::object();  // 激活时的输入快照
};

/// 业务动作基类
class NebulaAction {
public:
  virtual ~NebulaAction() = default;

  /// 端口声明(BT.CPP 语义)
  virtual BT::PortsList providedPorts() const = 0;

  /// 启动动作:
  /// - 返回 RUNNING:已注册 done,等待完成回调
  /// - 返回 SUCCESS / FAILURE:同步完成
  virtual BT::NodeStatus start(NebulaCtx& ctx, const Done& done) = 0;

  /// 取消在途工作(任务取消/树 halt 时回调;同步动作可不实现)
  virtual void halt(NebulaCtx& ctx) { (void)ctx; }
};

using NebulaActionPtr = std::shared_ptr<NebulaAction>;

} // namespace nebulabt
