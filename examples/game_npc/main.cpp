// ============================================================================
// game_npc:游戏 AI 示例 —— 守卫 NPC 的大脑
//
// 展示 NebulaBT 在游戏场景的典型用法:
//   - ReactiveFallback:反应式优先级("玩家靠近"随时打断巡逻)——游戏 AI 的
//     经典结构;NebulaBT 的事件驱动调度让"打断"零轮询
//   - WhileLoop:巡逻循环(waypoint 队列清空即正常退出)
//   - SwitchN:按 NPC 性格分发交互行为
//   - 异步动画/移动节点:发起后 RUNNING,完成回调 done(任意线程)
//   - LLMDecision(M8):LLM 只做"选分支",控制流仍在树里;幻觉分支被
//     节点边界拦截;ReactiveGuard 守护战斗段——"LLM 提议,守护裁决"
//
// 场景(每个一个任务,确定性断言):
//   A. 玩家远离  -> 巡逻全部 waypoint -> SUCCEEDED
//   B. 玩家靠近 + 友好性格 -> Greet
//   C. 玩家靠近 + 暴躁性格 -> Threaten
//   D. 巡逻途中玩家靠近 -> ReactiveFallback 打断巡逻(巡逻未走完)
//   E. LLM 驱动决策:E1 决策巡逻 / E2 决策交战并完成 / E3 交战中玩家
//      撤离 -> 守护违反 -> GUARD_VIOLATED(决策不可信,守护兜底)
// ============================================================================

#include <nebulabt/core/engine.hpp>
#include <nebulabt/nodes/llm_decision.hpp>
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

// ---- 场景 E:LLM 驱动决策 ----

// 感知:把世界状态写成黑板键(situation 文本 + in_combat 布尔)
// 守护段每 tick 重跑本节点 -> in_combat 恒新鲜(反应式感知刷新)
class SenseWorld : public NebulaAction {
public:
  explicit SenseWorld(std::shared_ptr<World> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override {
    return { BT::OutputPort<std::string>("situation"),
             BT::OutputPort<bool>("in_combat") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    const double d = world_->player_dist.load();
    ctx.out("situation", "guard at post; player_dist=" +
                             std::to_string(d) + "m");
    ctx.out("in_combat", d < 10.0);
    return BT::NodeStatus::SUCCESS;
  }

private:
  std::shared_ptr<World> world_;
};

// 交战(异步 150ms;守护违反时被 halt —— 断言依据)
class Engage : public NebulaAction {
public:
  explicit Engage(std::shared_ptr<World> world) : world_(std::move(world)) {}
  BT::PortsList providedPorts() const override { return {}; }
  BT::NodeStatus start(NebulaCtx&, const Done& done) override {
    world_->last_action = "Engage";
    std::cout << "  [npc] Engage\n";
    std::thread([this, done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      done(BT::NodeStatus::SUCCESS, {});
    }).detach();
    return BT::NodeStatus::RUNNING;
  }
  void halt(NebulaCtx&) override {
    halted_.store(true);
    std::cout << "  [npc] Engage halted (guard violation)\n";
  }
  bool halted() const { return halted_.load(); }

private:
  std::shared_ptr<World> world_;
  std::atomic<bool> halted_{false};
};

// LLM 决策树:决策在记忆式 Sequence 里只做一次;守护段用
// ReactiveSequence —— 守护巡检重 tick 时重跑 SenseWorld 刷新 in_combat,
// 但不会重复调用 LLM(v4 Sequence 对已完成子节点不重跑)
static const char* kLlmXml = R"(
<root BTCPP_format="v4" main_tree_to_execute="llm_brain">
  <BehaviorTree ID="llm_brain">
    <Sequence name="llm_brain">
      <SenseWorld situation="{situation}" in_combat="{in_combat}"/>
      <LLMDecision prompt="{situation}" branches="patrol,engage,retreat"
                   branch="{decision}" reason="{why}"/>
      <SwitchN variable="{decision}" case_1="patrol" case_2="engage"
               case_3="retreat">
        <WhileLoop max_iterations="50">
          <PatrolActive/>
          <VisitWaypoint waypoint="{wp}"/>
        </WhileLoop>
        <ReactiveSequence name="guarded_engage">
          <SenseWorld in_combat="{in_combat}"/>
          <ReactiveGuard while_true="{in_combat}" period_ms="30">
            <Engage/>
          </ReactiveGuard>
        </ReactiveSequence>
        <Retreat/>
      </SwitchN>
    </Sequence>
  </BehaviorTree>
</root>)";

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
  auto engage = std::make_shared<Engage>(world);
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

  // ==========================================================================
  // 场景 E:LLM 驱动决策(M8)
  //   LLM 客户端 = 规则 mock(读世界状态返回分支)——真实项目把这一层
  //   换成 HTTP/SDK 调用即可,树与节点代码零改动
  // ==========================================================================
  std::cout << "=== E: LLM-driven decisions ===\n";
  Engine llmEngine;
  llmEngine.registerAction("SenseWorld", std::make_shared<SenseWorld>(world));
  llmEngine.registerAction("PatrolActive", std::make_shared<PatrolActive>(world));
  llmEngine.registerAction("VisitWaypoint", std::make_shared<VisitWaypoint>(world));
  llmEngine.registerAction("Engage", engage);
  llmEngine.registerAction("Retreat", std::make_shared<Interaction>(world, "Retreat"));

  // 失败码登记(M8):先声明后使用;LLM 节点的失败码一并登记
  llmEngine.declareFailureCodes({
    {"LLM_INVALID_BRANCH", "LLM output not in allowed branch set"},
    {"LLM_NO_BRANCHES", "branches port empty"},
    {"ROUTE_DONE", "no waypoint left"},
  });

  // mock LLM:确定性规则——真实项目替换为 HTTP/SDK 客户端
  LlmDecision::Client llm = [world](const nlohmann::json& req, Done done) {
    std::thread([world, req, done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));  // 推理延迟
      const double d = world->player_dist.load();
      std::string branch, why;
      if (d < 5.0) {
        branch = "engage";
        why = "player within engagement range";
      } else if (world->patrolActive()) {
        branch = "patrol";
        why = "no threat; continue route";
      } else {
        branch = "retreat";
        why = "route finished; return to post";
      }
      std::cout << "  [llm] decision=" << branch << " (" << why << ")\n";
      done(BT::NodeStatus::SUCCESS,
           nlohmann::json{{"branch", branch}, {"reason", why}});
    }).detach();
  };
  llmEngine.registerAction("LLMDecision", std::make_shared<LlmDecision>(llm));

  // E1:玩家远离 -> LLM 选 patrol -> 走完全部 waypoint
  std::cout << "--- E1: far -> LLM picks patrol ---\n";
  world->reset();
  auto taskE1 = llmEngine.submitFromText(kLlmXml);
  CHECK(taskE1.waitFor(std::chrono::seconds(5)) == TaskState::Succeeded);
  CHECK(world->visited ==
        std::vector<std::string>({"gate", "plaza", "tower"}));

  // E2:玩家进入交战范围 -> LLM 选 engage -> 守护成立,交战完成
  std::cout << "--- E2: near -> LLM picks engage, guard holds ---\n";
  world->reset();
  world->player_dist.store(3.0);
  auto taskE2 = llmEngine.submitFromText(kLlmXml);
  CHECK(taskE2.waitFor(std::chrono::seconds(5)) == TaskState::Succeeded);
  CHECK(world->last_action == "Engage");
  CHECK(!engage->halted());

  // E3:交战中玩家撤离 -> 感知刷新 in_combat=false -> 守护裁决:
  //    LLM 的决策被否决,Engage 被 halt,任务 GUARD_VIOLATED
  //    —— "LLM 提议,守护裁决":决策可信的前提是守护条件持续成立
  std::cout << "--- E3: player flees mid-engage -> guard vetoes ---\n";
  world->reset();
  world->player_dist.store(3.0);
  std::thread([world] {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    world->player_dist.store(100.0);  // 玩家撤离(感知系统更新)
  }).detach();
  auto taskE3 = llmEngine.submitFromText(kLlmXml);
  CHECK(taskE3.waitFor(std::chrono::seconds(5)) == TaskState::Failed);
  CHECK(engage->halted());               // 在途交战被打断
  bool guardViolated = false;
  for (const auto& f : taskE3.failures()) {
    if (f.code == "GUARD_VIOLATED") guardViolated = true;
  }
  CHECK(guardViolated);

  // explain 轨迹:LLM 决策 + 守护裁决的完整证据链(审计用)
  const nlohmann::json ex = taskE3.explain();
  std::cout << "  [explain] task " << ex["task_id"].get<std::string>()
            << " state=" << ex["state"].get<std::string>()
            << ", failures=" << ex["failures"].size() << "\n";

  std::cout << "\ngame_npc PASSED\n";
  return 0;
}
