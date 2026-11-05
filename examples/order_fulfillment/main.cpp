// ============================================================================
// order_fulfillment:后端订单履约示例
//
// 展示 NebulaBT 在后端服务的典型用法:
//   - Compensable(Saga):扣库存声明补偿子树,发货失败时引擎自动逆序补偿
//   - Fallback 通道级兜底:主支付通道重试耗尽 → 备用通道接管
//   - RetryWithDelay 墙钟预算:支付重试不被无限拖死
//   - Checkpoint:库存已扣减是业务安全点,失败恢复时不重复扣减
//   - SwitchN:按订单档位(express/standard)分发发货通道
//   - schema:订单参数加载期校验
//
// 三个场景(全部 mock,确定性断言):
//   A. 正常履约:扣库存 → 主通道支付 → express 发货 → SUCCEEDED(无补偿)
//   B. 主通道持续故障 → 重试耗尽 → 备用通道接管 → SUCCEEDED
//   C. 发货失败 → 引擎自动执行 Compensable 补偿,释放库存 → FAILED
// ============================================================================

#include <nebulabt/core/engine.hpp>
#include <nebulabt/sdk/action.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace nebulabt;

// ---------------------------------------------------------------------------
// MockWorld:库存/支付/物流三个子系统的内存替身
// ---------------------------------------------------------------------------
struct World {
  std::map<std::string, int> stock;            // sku -> 数量
  std::atomic<int> charge_attempts{0};         // 主通道调用次数(含重试)
  std::atomic<bool> primary_down{false};       // 主支付通道是否故障
  std::atomic<bool> ship_fail{false};          // 发货是否失败
  std::atomic<bool> reserved{false};           // 库存是否已扣减
  std::atomic<bool> released{false};           // 是否触发补偿释放
  std::string paid_via;

  void reset() {
    stock.clear();
    charge_attempts.store(0);
    primary_down.store(false);
    ship_fail.store(false);
    reserved.store(false);
    released.store(false);
    paid_via.clear();
  }
};

// ---------------------------------------------------------------------------
// 业务节点
// ---------------------------------------------------------------------------

// 扣减库存(副作用:必须幂等或受 Checkpoint 保护)
class ReserveStock : public NebulaAction {
public:
  explicit ReserveStock(std::shared_ptr<World> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx&, const Done&) override {
    world_->reserved.store(true);
    return BT::NodeStatus::SUCCESS;
  }

private:
  std::shared_ptr<World> world_;
};

// 校验每个 sku 有货(由 ForEach 逐个调用)
class CheckItem : public NebulaAction {
public:
  explicit CheckItem(std::shared_ptr<World> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("sku") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    const std::string sku = ctx.in<std::string>("sku");
    if (world_->stock[sku] <= 0) {
      ctx.fail("OUT_OF_STOCK", "sku sold out: " + sku);
      return BT::NodeStatus::FAILURE;
    }
    return BT::NodeStatus::SUCCESS;
  }

private:
  std::shared_ptr<World> world_;
};

// 支付(异步;provider 端口区分主/备通道)
class ChargePayment : public NebulaAction {
public:
  explicit ChargePayment(std::shared_ptr<World> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("provider") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    const std::string provider = ctx.in<std::string>("provider");
    if (provider == "primary") {
      world_->charge_attempts.fetch_add(1);
    }
    if (provider == "primary" && world_->primary_down.load()) {
      ctx.fail("PROVIDER_DOWN", "primary payment provider unavailable");
      std::thread([done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        done(BT::NodeStatus::FAILURE, {});
      }).detach();
    } else {
      std::thread([this, provider, done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        world_->paid_via = provider;
        done(BT::NodeStatus::SUCCESS, {});
      }).detach();
    }
    return BT::NodeStatus::RUNNING;
  }

private:
  std::shared_ptr<World> world_;
};

// 发货(异步;可注入失败)
class ShipOrder : public NebulaAction {
public:
  explicit ShipOrder(std::shared_ptr<World> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    if (world_->ship_fail.load()) {
      ctx.fail("SHIP_FAILED", "carrier rejected the shipment");
      std::thread([done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        done(BT::NodeStatus::FAILURE, {});
      }).detach();
    } else {
      std::thread([done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        done(BT::NodeStatus::SUCCESS, {});
      }).detach();
    }
    return BT::NodeStatus::RUNNING;
  }

private:
  std::shared_ptr<World> world_;
};

// 补偿:释放库存(失败兜底路径)
class ReleaseStock : public NebulaAction {
public:
  explicit ReleaseStock(std::shared_ptr<World> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx&, const Done&) override {
    world_->released.store(true);
    std::cout << "  [compensate] stock reservation released\n";
    return BT::NodeStatus::SUCCESS;
  }

private:
  std::shared_ptr<World> world_;
};

// ---------------------------------------------------------------------------
// 履约树
//   Compensable(Saga):扣库存声明补偿,发货失败时引擎自动逆序补偿——
//     手写补偿分支从树里消失,"扣了库存忘了释放"在结构上不可能发生
//   支付段:主通道带预算重试,耗尽落到备用通道(通道级降级)
// ---------------------------------------------------------------------------
static const char* kXml = R"(
<root BTCPP_format="v4" main_tree_to_execute="fulfill_order">
  <BehaviorTree ID="fulfill_order">
    <Sequence name="fulfill">
      <ForEach array="{items}" element="{sku}">
        <CheckItem sku="{sku}"/>
      </ForEach>
      <Compensable id="reserve_stock">
        <Checkpoint id="reserved">
          <ReserveStock/>
        </Checkpoint>
        <ReleaseStock/>
      </Compensable>
      <Checkpoint id="paid">
        <Fallback name="payment">
          <RetryWithDelay attempts="3" delay_ms="30" budget_ms="2000">
            <ChargePayment provider="primary"/>
          </RetryWithDelay>
          <ChargePayment provider="backup"/>
        </Fallback>
      </Checkpoint>
      <SwitchN variable="{tier}" case_1="express">
        <ShipOrder/>
        <ShipOrder/>
      </SwitchN>
    </Sequence>
  </BehaviorTree>
</root>)";

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "CHECK failed: " #cond " (line " << __LINE__ << ")\n";     \
      std::cout << "\norder_fulfillment FAILED\n";                             \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

static TaskState runOrder(Engine& engine, const std::string& tier) {
  TaskSpec spec;
  spec.task_type = "FULFILL_ORDER";
  spec.slots["tier"] = tier;
  spec.slots["items"] = {"sku-1", "sku-2"};
  // schema:订单参数加载期校验(生产中由 pack 声明,此处内联演示)
  spec.schema = {
    { "slots", {
        { "tier",  { { "type", "string" },       { "required", true } } },
        { "items", { { "type", "string_array" }, { "required", false } } },
    } }
  };
  auto task = engine.submitFromText(kXml, spec);
  const TaskState state = task.wait();
  std::cout << "  state: " << toString(state) << "\n";
  for (const auto& f : task.failures()) {
    std::cout << "    - [" << f.code << "] " << f.node << ": " << f.message
              << "\n";
  }
  return state;
}

int main() {
  auto world = std::make_shared<World>();
  Engine engine;
  engine.registerAction("CheckItem", std::make_shared<CheckItem>(world));
  engine.registerAction("ReserveStock", std::make_shared<ReserveStock>(world));
  engine.registerAction("ChargePayment", std::make_shared<ChargePayment>(world));
  engine.registerAction("ShipOrder", std::make_shared<ShipOrder>(world));
  engine.registerAction("ReleaseStock", std::make_shared<ReleaseStock>(world));
  engine.setLogger([](LogLevel, const std::string& msg) {
    std::cout << "  [engine] " << msg << "\n";
  });

  // ---- 场景 A:正常履约(主通道一次成功)----
  std::cout << "=== A: normal order, express tier ===\n";
  world->stock = {{"sku-1", 5}, {"sku-2", 5}};
  CHECK(runOrder(engine, "express") == TaskState::Succeeded);
  CHECK(world->charge_attempts.load() == 1);      // 主通道一次成功,无重试
  CHECK(world->paid_via == "primary");
  CHECK(world->released.load() == false);         // 未触发补偿

  // ---- 场景 B:主通道持续故障 → 备用通道接管 ----
  std::cout << "\n=== B: primary down, backup takes over ===\n";
  world->reset();
  world->stock = {{"sku-1", 5}, {"sku-2", 5}};
  world->primary_down.store(true);
  CHECK(runOrder(engine, "standard") == TaskState::Succeeded);
  CHECK(world->charge_attempts.load() == 3);      // 主通道重试 3 次(预算内)
  CHECK(world->paid_via == "backup");             // 备用通道接管

  // ---- 场景 C:发货失败 → 补偿释放库存 ----
  std::cout << "\n=== C: ship fails -> compensation ===\n";
  world->reset();
  world->stock = {{"sku-1", 5}, {"sku-2", 5}};
  world->ship_fail.store(true);
  CHECK(runOrder(engine, "express") == TaskState::Failed);
  CHECK(world->reserved.load() == true);          // 库存确实扣过
  CHECK(world->released.load() == true);          // 补偿路径释放了库存

  std::cout << "\norder_fulfillment PASSED\n";
  return 0;
}
