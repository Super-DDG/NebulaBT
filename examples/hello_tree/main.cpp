// NebulaBT Hello Tree:5 分钟上手示例。
// 覆盖:同步节点、异步节点(跨线程 done 回调)、ForEach 动态展开、
//       slots 注入、explain 执行报告。

#include <nebulabt/core/engine.hpp>
#include <nebulabt/sdk/action.hpp>

#include <chrono>
#include <iostream>
#include <thread>

using namespace nebulabt;

// ---------------------------------------------------------------------------
// 同步节点:直接返回终态
// ---------------------------------------------------------------------------
class Greet : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("who") };
  }

  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    std::cout << "hello, " << ctx.in<std::string>("who") << "!\n";
    return BT::NodeStatus::SUCCESS;
  }
};

// ---------------------------------------------------------------------------
// 异步节点:发起调用返回 RUNNING,100ms 后在另一个线程回调 done。
// result 中的 "price" 键会写回 Output 端口。
// ---------------------------------------------------------------------------
class FetchPrice : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return {
      BT::InputPort<std::string>("symbol"),
      BT::OutputPort<double>("price"),
    };
  }

  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    const std::string symbol = ctx.in<std::string>("symbol");
    std::thread([symbol, done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      // 模拟 RPC 返回;此处可在任意线程
      const double price = 3.14 + static_cast<double>(symbol.size()) * 0.01;
      done(BT::NodeStatus::SUCCESS, { { "price", price } });
    }).detach();
    return BT::NodeStatus::RUNNING;
  }
};

int main() {
  Engine engine;
  engine.registerAction("Greet", std::make_shared<Greet>());
  engine.registerAction("FetchPrice", std::make_shared<FetchPrice>());

  static const char* kXml = R"(
  <root BTCPP_format="v4">
    <BehaviorTree ID="hello">
      <Sequence name="hello_main">
        <Greet who="{user}"/>
        <ForEach array="{watchlist}" element="{symbol}">
          <FetchPrice symbol="{symbol}" price="{price}"/>
        </ForEach>
        <Greet who="world"/>
      </Sequence>
    </BehaviorTree>
  </root>)";

  TaskSpec spec;
  spec.task_type = "PRICE_CHECK";
  spec.slots["user"] = "nebula";
  spec.slots["watchlist"] = { "AAPL", "TSLA", "NVDA" };

  auto task = engine.submitFromText(kXml, spec);
  task.wait();

  std::cout << "\n=== explain() ===\n" << task.explain().dump(2) << "\n";
  return task.state() == TaskState::Succeeded ? 0 : 1;
}
