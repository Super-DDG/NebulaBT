// ============================================================================
// robot_delivery:M1 试金石示例
//
// 演示一个典型的"拣选-递送"业务流程(常见于机器人/物流场景),验收两个目标:
//   1. 零复制:静态行为树 XML 中,"导航+检测+确认"这类子树按地点复制多份
//      是常见反模式;这里收敛为 1 份,由 ForEach 数据驱动。
//      地点清单是运行时数据(PlanLocations 输出),不是 XML 里的硬编码 case。
//   2. 失败可解释:每一步失败都有结构化记录,explain() 直接还原
//      "这单为什么失败/为什么跑了 N 次抓取"。
//
// 两个场景:
//   A. 递送水瓶(在 shelf_b):shelf_a 没找到 -> 跳过 -> shelf_b 找到
//      -> 抓取失败 2 次 -> 第 3 次成功 -> 递交。
//      终态 SUCCEEDED,explain 里有 2 条 GRASP_FAILED。
//   B. 递送不存在的物品:所有地点未找到 -> NOT_FOUND -> FAILED。
//
// 真实项目中 NavigateTo/DetectItem/GraspItem 等节点内部对接具体系统的
// Action/RPC;此处用 MockWorld 模拟,业务树与节点逻辑不变。
// ============================================================================

#include <nebulabt/core/engine.hpp>
#include <nebulabt/sdk/action.hpp>

#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace nebulabt;

// ---------------------------------------------------------------------------
// MockWorld:模拟能力层(真实项目中是各系统的 Adapter)
// ---------------------------------------------------------------------------
struct MockWorld {
  std::set<std::string> reachable{"shelf_a", "shelf_b", "handover_point"};
  // 地点 -> (物品提示词 -> 数量)
  std::map<std::string, std::map<std::string, int>> vision;
  int grasp_fail_times = 0;  // 前 N 次抓取失败(演示重试挽回)
  int grasp_attempts = 0;

  int countAt(const std::string& loc, const std::string& prompt) const {
    auto it = vision.find(loc);
    if (it == vision.end()) return 0;
    auto pit = it->second.find(prompt);
    return pit == it->second.end() ? 0 : pit->second;
  }

  void reset() {
    grasp_attempts = 0;
    grasp_fail_times = 0;
  }
};

// ---------------------------------------------------------------------------
// 业务节点:每个都是"一段业务逻辑",不感知 tick/线程/轮询
// ---------------------------------------------------------------------------

// 前置检查(真实项目:各 Adapter 存活状态)
class CheckAdapters : public NebulaAction {
public:
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx&, const Done&) override {
    return BT::NodeStatus::SUCCESS;
  }
};

// 物品名 -> 视觉提示词(真实项目中通常来自配置表)
class ResolveItem : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return {
      BT::InputPort<std::string>("target_item"),
      BT::OutputPort<std::string>("prompt"),
      BT::OutputPort<std::string>("display_name"),
    };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    const std::string item = ctx.in<std::string>("target_item");
    std::string prompt, displayName;
    if (item == "water_bottle") {
      prompt = "water bottle";
      displayName = "water bottle";
    } else if (item == "toy_duck") {
      prompt = "toy duck";
      displayName = "toy duck";
    } else {
      ctx.fail("UNKNOWN_ITEM", "no prompt mapping for item: " + item);
      return BT::NodeStatus::FAILURE;
    }
    ctx.out("prompt", prompt);
    ctx.out("display_name", displayName);
    return BT::NodeStatus::SUCCESS;
  }
};

// 目标地点 -> 候选地点清单(数据驱动,替代原 XML 里的 SwitchN 硬编码 case)
class PlanLocations : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return {
      BT::InputPort<std::string>("target_location"),
      BT::OutputPort<std::vector<std::string>>("locations"),
    };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    const std::string loc = ctx.in<std::string>("target_location");
    std::vector<std::string> locations;
    if (loc == "shelf_a" || loc == "shelf_b") {
      locations = {loc};                     // 指定地点:只去一处
    } else {
      locations = {"shelf_a", "shelf_b"};    // 空/未知:依次尝试
    }
    ctx.out("locations", locations);
    return BT::NodeStatus::SUCCESS;
  }
};

// 导航(异步:发起后 RUNNING,完成回调唤醒)
class NavigateTo : public NebulaAction {
public:
  explicit NavigateTo(std::shared_ptr<MockWorld> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("point_id") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    const std::string pointId = ctx.in<std::string>("point_id");
    if (!world_->reachable.count(pointId)) {
      ctx.fail("NAV_POINT_UNKNOWN", "unknown navigation point: " + pointId);
      return BT::NodeStatus::FAILURE;
    }
    std::thread([done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      done(BT::NodeStatus::SUCCESS, {});
    }).detach();
    return BT::NodeStatus::RUNNING;
  }

private:
  std::shared_ptr<MockWorld> world_;
};

// 视觉检测(同步)
class DetectItem : public NebulaAction {
public:
  explicit DetectItem(std::shared_ptr<MockWorld> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override {
    return {
      BT::InputPort<std::string>("loc"),
      BT::InputPort<std::string>("prompt"),
      BT::OutputPort<int>("count"),
      BT::OutputPort<std::string>("first_id"),
    };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    const int count = world_->countAt(ctx.in<std::string>("loc"),
                                      ctx.in<std::string>("prompt"));
    ctx.out("count", count);
    ctx.out("first_id", count > 0 ? ctx.in<std::string>("prompt") + "#1" : "");
    return BT::NodeStatus::SUCCESS;
  }

private:
  std::shared_ptr<MockWorld> world_;
};

// 确认本地点找到(找不到则失败,由 ForEach continue_on_failure 跳到下一地点)
class RequireFound : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return {
      BT::InputPort<int>("count"),
      BT::InputPort<std::string>("display_name"),
      BT::InputPort<std::string>("loc"),
    };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    if (ctx.in<int>("count", 0) <= 0) {
      ctx.fail("NOT_FOUND", "no " + ctx.in<std::string>("display_name") +
                                " at " + ctx.in<std::string>("loc"));
      return BT::NodeStatus::FAILURE;
    }
    return BT::NodeStatus::SUCCESS;
  }
};

// 全部地点搜完后的最终确认
class AssertFound : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return {
      BT::InputPort<int>("count"),
      BT::InputPort<std::string>("display_name"),
    };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    if (ctx.in<int>("count", 0) <= 0) {
      ctx.fail("NOT_FOUND", "searched all locations, " +
                                ctx.in<std::string>("display_name") +
                                " not found");
      return BT::NodeStatus::FAILURE;
    }
    return BT::NodeStatus::SUCCESS;
  }
};

// 抓取(异步;演示前 N 次失败由 RetryWithDelay 挽回)
class GraspItem : public NebulaAction {
public:
  explicit GraspItem(std::shared_ptr<MockWorld> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("object_id") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    const int attempt = ++world_->grasp_attempts;
    if (attempt <= world_->grasp_fail_times) {
      ctx.fail("GRASP_FAILED",
               "gripper timeout, attempt " + std::to_string(attempt));
      std::thread([done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        done(BT::NodeStatus::FAILURE, {});
      }).detach();
    } else {
      std::thread([done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        done(BT::NodeStatus::SUCCESS, {});
      }).detach();
    }
    return BT::NodeStatus::RUNNING;
  }

private:
  std::shared_ptr<MockWorld> world_;
};

// 递交(异步)
class HandoverItem : public NebulaAction {
public:
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx&, const Done& done) override {
    std::thread([done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      done(BT::NodeStatus::SUCCESS, {});
    }).detach();
    return BT::NodeStatus::RUNNING;
  }
};

// TTS 播报(同步)
class Speak : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("text") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    std::cout << "  [TTS] " << ctx.in<std::string>("text") << "\n";
    return BT::NodeStatus::SUCCESS;
  }
};

// ---------------------------------------------------------------------------
// 业务树:搜寻段 1 份定义,由 ForEach 按数据驱动 N 次(对比常见的 4 份复制)
// ---------------------------------------------------------------------------
static const char* kXml = R"(
<root BTCPP_format="v4" main_tree_to_execute="pick_and_deliver">
  <BehaviorTree ID="pick_and_deliver">
    <Sequence name="deliver_main">
      <CheckAdapters/>
      <ResolveItem target_item="{target_item}" prompt="{prompt}" display_name="{display_name}"/>
      <PlanLocations target_location="{target_location}" locations="{locations}"/>

      <!-- 零复制核心:同一份"导航+检测+确认"逻辑,数据驱动遍历所有候选地点 -->
      <ForEach array="{locations}" element="{loc}" continue_on_failure="true">
        <Sequence name="search_at">
          <NavigateTo point_id="{loc}"/>
          <DetectItem loc="{loc}" prompt="{prompt}" count="{count}" first_id="{oid}"/>
          <RequireFound count="{count}" display_name="{display_name}" loc="{loc}"/>
        </Sequence>
      </ForEach>

      <AssertFound count="{count}" display_name="{display_name}"/>
      <Speak text="Found the item. Starting grasp."/>
      <RetryWithDelay attempts="{grasp_retry}" delay_ms="300">
        <Sequence>
          <DetectItem loc="{loc}" prompt="{prompt}" count="{count}" first_id="{oid}"/>
          <GraspItem object_id="{oid}"/>
        </Sequence>
      </RetryWithDelay>

      <NavigateTo point_id="handover_point"/>
      <Speak text="Here is your item, please take it."/>
      <HandoverItem/>
      <Speak text="Task completed."/>
    </Sequence>
  </BehaviorTree>
</root>)";

// ---------------------------------------------------------------------------
// 场景驱动
// ---------------------------------------------------------------------------
static TaskState runDelivery(Engine& engine, MockWorld& world,
                             const std::string& item,
                             const std::string& location, int graspRetry) {
  TaskSpec spec;
  spec.task_type = "DELIVER_ORDER";
  spec.timeout_ms = 60000;  // 任务级超时兜底(策略引擎)

  // schema(M1):加载期校验 —— required 缺失 / 未知字段 / 类型不符
  // / 树中引用未声明键,全部在提交时拒绝;int 类型按真实类型注入黑板
  spec.schema = {
    { "slots", {
        { "target_item",     { { "type", "string" }, { "required", true } } },
        { "target_location", { { "type", "string" }, { "required", false } } },
        { "grasp_retry",     { { "type", "int" },    { "required", false } } },
    } }
  };

  spec.slots["target_item"] = item;
  spec.slots["target_location"] = location;
  spec.slots["grasp_retry"] = graspRetry;

  auto task = engine.submitFromText(kXml, spec);
  const TaskState state = task.wait();

  std::cout << "  state: " << toString(state) << "\n";
  std::cout << "  failures:\n";
  for (const auto& f : task.failures()) {
    std::cout << "    - [" << f.code << "] " << f.node << ": " << f.message
              << "\n";
  }
  return state;
}

int main() {
  auto world = std::make_shared<MockWorld>();
  Engine engine;

  engine.registerAction("CheckAdapters", std::make_shared<CheckAdapters>());
  engine.registerAction("ResolveItem", std::make_shared<ResolveItem>());
  engine.registerAction("PlanLocations", std::make_shared<PlanLocations>());
  engine.registerAction("NavigateTo", std::make_shared<NavigateTo>(world));
  engine.registerAction("DetectItem", std::make_shared<DetectItem>(world));
  engine.registerAction("RequireFound", std::make_shared<RequireFound>());
  engine.registerAction("AssertFound", std::make_shared<AssertFound>());
  engine.registerAction("GraspItem", std::make_shared<GraspItem>(world));
  engine.registerAction("HandoverItem", std::make_shared<HandoverItem>());
  engine.registerAction("Speak", std::make_shared<Speak>());

  engine.setLogger([](LogLevel, const std::string& msg) {
    std::cout << "  [engine] " << msg << "\n";
  });

  // 场景 A:递送水瓶(实际在 shelf_b);shelf_a 未找到被跳过,抓取失败 2 次后挽回
  std::cout << "=== Scenario A: deliver water_bottle (grasp fails twice) ===\n";
  world->vision["shelf_b"]["water bottle"] = 2;
  world->grasp_fail_times = 2;
  const TaskState stateA = runDelivery(engine, *world, "water_bottle", "", 5);

  // 场景 B:递送不存在的物品:所有地点 NOT_FOUND -> FAILED
  std::cout << "\n=== Scenario B: deliver toy_duck (does not exist) ===\n";
  world->reset();
  const TaskState stateB = runDelivery(engine, *world, "toy_duck", "", 3);

  const bool ok = (stateA == TaskState::Succeeded) &&
                  (stateB == TaskState::Failed);
  std::cout << "\nrobot_delivery " << (ok ? "PASSED" : "FAILED") << "\n";
  return ok ? 0 : 1;
}
