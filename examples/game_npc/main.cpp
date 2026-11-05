// ============================================================================
// game_npc:游戏 AI 示例 —— 守卫 NPC 的大脑
//
// 展示 NebulaBT 在游戏场景的典型用法:
//   - ReactiveFallback:反应式优先级("玩家靠近"随时打断巡逻)——游戏 AI 的
//     经典结构;NebulaBT 的事件驱动调度让"打断"零轮询
//   - WhileLoop:巡逻循环(waypoint 队列清空即正常退出)
//   - SwitchN:按 NPC 性格分发交互行为
//   - 异步动画/移动节点:发起后 RUNNING,完成回调 done(任意线程)
//
// 四个场景(每个一个任务,确定性断言):
//   A. 玩家远离  -> 巡逻全部 waypoint -> SUCCEEDED
//   B. 玩家靠近 + 友好性格 -> Greet
//   C. 玩家靠近 + 暴躁性格 -> Threaten
//   D. 巡逻途中玩家靠近 -> ReactiveFallback 打断巡逻(巡逻未走完)
// ============================================================================

#include <nebulabt/core/engine.hpp>
#include <nebulabt/sdk/action.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace nebulabt;

// ---------------------------------------------------------------------------
// MockWorld:游戏世界(真实项目中由引擎帧循环/感知系统驱动)
// ---------------------------------------------------------------------------
struct World {
  std::vector<std::string> route{"gate", "plaza", "tower"};
  size_t next = 0;
  std::atomic<double> player_dist{100.0};  // 玩家距离(米);感知线程会更新
  std::vector<std::string> visited;
  std::string last_action;

  bool playerNear() const { return player_dist.load() < 5.0; }
  bool patrolActive() const { return next < route.size(); }
  void reset() {
    next = 0;
    visited.clear();
    last_action.clear();
    player_dist.store(100.0);
  }
};

// ---------------------------------------------------------------------------
// 业务节点
// ---------------------------------------------------------------------------

// 条件:玩家是否靠近
class PlayerNear : public NebulaAction {
public:
  explicit PlayerNear(std::shared_ptr<World> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx&, const Done&) override {
    return world_->playerNear() ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
  }

private:
  std::shared_ptr<World> world_;
};

// 条件:还有巡逻点
class PatrolActive : public NebulaAction {
public:
  explicit PatrolActive(std::shared_ptr<World> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx&, const Done&) override {
    return world_->patrolActive() ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
  }

private:
  std::shared_ptr<World> world_;
};

// 走向下一个巡逻点(异步:80ms 后到达)
class VisitWaypoint : public NebulaAction {
public:
  explicit VisitWaypoint(std::shared_ptr<World> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override {
    return { BT::OutputPort<std::string>("waypoint") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    if (!world_->patrolActive()) {
      ctx.fail("ROUTE_DONE", "no waypoint left");
      return BT::NodeStatus::FAILURE;
    }
    const std::string wp = world_->route[world_->next++];
    ctx.out("waypoint", wp);
    std::thread([this, wp, done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));  // 走路耗时
      world_->visited.push_back(wp);
      done(BT::NodeStatus::SUCCESS, {});
    }).detach();
    return BT::NodeStatus::RUNNING;
  }

private:
  std::shared_ptr<World> world_;
};

// 交互动作(同步,记录最后行为供断言)
class Interaction : public NebulaAction {
public:
  Interaction(std::shared_ptr<World> world, std::string name)
    : world_(std::move(world)), name_(std::move(name)) {}
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx&, const Done&) override {
    world_->last_action = name_;
    std::cout << "  [npc] " << name_ << "\n";
    return BT::NodeStatus::SUCCESS;
  }

private:
  std::shared_ptr<World> world_;
  std::string name_;
};

// 播放动画(异步)
class PlayAnimation : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("anim") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    std::cout << "  [anim] " << ctx.in<std::string>("anim") << "\n";
    std::thread([done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      done(BT::NodeStatus::SUCCESS, {});
    }).detach();
    return BT::NodeStatus::RUNNING;
  }
};

// ---------------------------------------------------------------------------
// 大脑树:ReactiveFallback = 反应式优先级
//   高优先分支:玩家靠近 → 播动画 → SwitchN 按性格分发(每轮重估,随时打断巡逻)
//   低优先分支:WhileLoop 巡逻(waypoint 走完自然退出)
// ---------------------------------------------------------------------------
static const char* kXml = R"(
<root BTCPP_format="v4" main_tree_to_execute="guard_brain">
  <BehaviorTree ID="guard_brain">
    <ReactiveFallback name="brain">

      <Sequence name="react_to_player">
        <PlayerNear/>
        <PlayAnimation anim="{mood}_anim"/>
        <SwitchN variable="{mood}" case_1="friendly" case_2="grumpy">
          <Greet/>
          <Threaten/>
          <Wander/>
        </SwitchN>
      </Sequence>

      <WhileLoop max_iterations="50">
        <PatrolActive/>
        <VisitWaypoint waypoint="{wp}"/>
      </WhileLoop>

    </ReactiveFallback>
  </BehaviorTree>
</root>)";

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "CHECK failed: " #cond " (line " << __LINE__ << ")\n";     \
      std::cout << "\ngame_npc FAILED\n";                                      \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

int main() {
  auto world = std::make_shared<World>();
  Engine engine;
  engine.registerAction("PlayerNear", std::make_shared<PlayerNear>(world));
  engine.registerAction("PatrolActive", std::make_shared<PatrolActive>(world));
  engine.registerAction("VisitWaypoint", std::make_shared<VisitWaypoint>(world));
  engine.registerAction("PlayAnimation", std::make_shared<PlayAnimation>());
  engine.registerAction("Greet", std::make_shared<Interaction>(world, "Greet"));
  engine.registerAction("Threaten",
                        std::make_shared<Interaction>(world, "Threaten"));
  engine.registerAction("Wander", std::make_shared<Interaction>(world, "Wander"));
  engine.setLogger([](LogLevel, const std::string& msg) {
    std::cout << "  [engine] " << msg << "\n";
  });

  // ---- 场景 A:玩家远离,巡逻全部 waypoint ----
  std::cout << "=== A: patrol while player far ===\n";
  auto taskA = engine.submitFromText(kXml);
  CHECK(taskA.waitFor(std::chrono::seconds(5)) == TaskState::Succeeded);
  CHECK(world->visited ==
        std::vector<std::string>({"gate", "plaza", "tower"}));

  // ---- 场景 B:玩家靠近 + 友好 -> Greet(SwitchN case_1)----
  std::cout << "=== B: player near, friendly -> Greet ===\n";
  world->reset();
  world->player_dist.store(2.0);
  TaskSpec specB;
  specB.slots["mood"] = "friendly";
  auto taskB = engine.submitFromText(kXml, specB);
  CHECK(taskB.waitFor(std::chrono::seconds(5)) == TaskState::Succeeded);
  CHECK(world->last_action == "Greet");

  // ---- 场景 C:玩家靠近 + 暴躁 -> Threaten(SwitchN case_2)----
  std::cout << "=== C: player near, grumpy -> Threaten ===\n";
  world->reset();
  world->player_dist.store(2.0);
  TaskSpec specC;
  specC.slots["mood"] = "grumpy";
  auto taskC = engine.submitFromText(kXml, specC);
  CHECK(taskC.waitFor(std::chrono::seconds(5)) == TaskState::Succeeded);
  CHECK(world->last_action == "Threaten");

  // ---- 场景 D:巡逻途中玩家靠近 -> ReactiveFallback 打断巡逻 ----
  // waypoint 单程 80ms;100ms 后感知系统报告玩家进入 5m
  std::cout << "=== D: interrupt patrol on player approach ===\n";
  world->reset();
  std::thread([world] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    world->player_dist.store(2.0);  // 真实项目中由感知系统更新
  }).detach();
  TaskSpec specD;
  specD.slots["mood"] = "friendly";
  auto taskD = engine.submitFromText(kXml, specD);
  CHECK(taskD.waitFor(std::chrono::seconds(5)) == TaskState::Succeeded);
  CHECK(world->last_action == "Greet");        // 打断后走了交互分支
  CHECK(world->visited.size() < 3);            // 巡逻未走完即被打断

  std::cout << "\ngame_npc PASSED\n";
  return 0;
}
