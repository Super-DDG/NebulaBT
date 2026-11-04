# ADR-002: 回调续传节点 API(Done)

- 状态:已接受
- 日期:2026-11-04

## 背景

C++17 没有协程(见 ADR-001),而业务节点天然是异步的:RPC、DB、消息、
机械臂动作……节点 API 必须让"发起调用→等待完成→应用结果"这条链路
对作者尽量透明,同时不引入轮询。

## 决策

业务动作继承 `NebulaAction`,以**回调续传**表达异步:

```cpp
class FetchPrice : public NebulaAction {
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("symbol"),
             BT::OutputPort<double>("price") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    // 发起调用(真实场景是 RPC),返回 RUNNING
    rpcAsync(ctx.in<std::string>("symbol"),
             [done](Result r) {                 // 任意线程回调
               done(BT::NodeStatus::SUCCESS, {{"price", r.price}});
             });
    return BT::NodeStatus::RUNNING;
  }
};
```

- 同步动作:`start` 直接返回 SUCCESS/FAILURE
- 异步动作:返回 RUNNING;完成时在**任意线程**调用 `done(status, result)`
- 取消:框架 halt 时回调 `halt(ctx)`,动作应取消在途工作
- `result` 中的键由框架按端口声明**类型化写回** Output 端口

## 实现要点

桥接节点(`detail::BridgeNode`,继承 `StatefulActionNode`):

- `done` 的结果暂存于 `Slot`(shared_ptr,生命周期独立于节点),
  再 `post` 到调度器唤醒 tick——跨线程安全,且不轮询
- 同步完成(start 内直接调 done 后返回终态)以返回值为准
- 重复 done 只认第一次
- 节点异常被捕获并转为结构化失败(`ACTION_EXCEPTION`)

## 备选方案与否决理由

- **C++20 协程**:语言标准不允许(ADR-001)
- **onRunning 轮询状态**(BT.CPP 传统写法):每个节点都要写轮询样板,
  且唤醒延迟取决于轮询间隔;回调续传让"完成即唤醒"成为框架保证
- **std::future + wait_for**:同样落入轮询,且 future 不可组合
