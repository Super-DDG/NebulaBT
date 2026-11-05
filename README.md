# NebulaBT · 星云树

**Collapse chaos into order.**

> 星云,是宇宙中最混沌的物质;它坍缩,才成为恒星。
> 业务逻辑也是一团星云——NebulaBT 给它们引力,让混沌坍缩成一棵清晰的树。

NebulaBT turns behavior trees from a game-AI specialty into an everyday tool
for business engineers. It organizes complex business logic in a way that is
**declarative, explainable, recoverable and testable**.

Write business logic, not glue code.

## Why

Long if-else chains, opaque call paths, state scattered across modules — every
large codebase grows this way until nobody dares to touch it. Behavior trees
already solved the *structural* problem in games and robotics; NebulaBT makes
that solution usable for everyone else.

| Highlight | Pain → Cure |
|---|---|
| Write business, not glue | C++17 callback-continuation nodes; async is native, no polling |
| Fail at load time, not at 3 a.m. | Typed blackboard + tree ↔ schema cross-validation |
| Failures speak human | Structured failure records + `explain(task_id)` reconstructs "why did this request take that path" |
| Safety you cannot forget | Timeouts, retry budgets and cancel propagation wrapped by the engine, not by authors |
| A crash is not the end | Event journal + checkpoints + idempotency → resume |
| Trees are assets, not copies | Template trees + instance overrides; versioned tree packs |
| Testing is recording | Mocks generated from the same capability interface; replay production journals as regression tests |
| One picture for the product team | Trees generate their own flow documentation |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│ Industry adapters   ROS2 │ game engines │ HTTP / gRPC / DB   │
├─────────────────────────────────────────────────────────────┤
│ Capability layer    NebulaAction nodes + Mock generation     │
├─────────────────────────────────────────────────────────────┤
│ NebulaBT core       Scheduler (event-driven)                │
│                     Lifecycle (task state machine)           │
│                     Policy (timeout / retry / preemption)    │
│                     Failure (structured propagation)         │
│                     Journal (event sourcing, resume)         │
│                     Registry (tree packs, versioning)        │
├─────────────────────────────────────────────────────────────┤
│ Tree engine         BehaviorTree.CPP v4 (unmodified)         │
└─────────────────────────────────────────────────────────────┘
```

## Quick start

```cpp
#include <nebulabt/core/engine.hpp>
#include <nebulabt/sdk/action.hpp>

using namespace nebulabt;

// A synchronous node
class Greet : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("who") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    std::cout << "hello, " << ctx.in<std::string>("who") << "\n";
    return BT::NodeStatus::SUCCESS;
  }
};

// An async node: fire the call, return RUNNING, invoke done() from any thread
class FetchPrice : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("symbol"),
             BT::OutputPort<double>("price") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    std::thread([symbol = ctx.in<std::string>("symbol"), done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      done(BT::NodeStatus::SUCCESS, {{"price", 3.14}});
    }).detach();
    return BT::NodeStatus::RUNNING;
  }
};

int main() {
  Engine engine;
  engine.registerAction("Greet", std::make_shared<Greet>());
  engine.registerAction("FetchPrice", std::make_shared<FetchPrice>());

  const char* xml = R"(
  <root BTCPP_format="v4">
    <BehaviorTree ID="hello">
      <Sequence>
        <Greet who="{user}"/>
        <ForEach array="{watchlist}" element="{symbol}">
          <FetchPrice symbol="{symbol}" price="{price}"/>
        </ForEach>
      </Sequence>
    </BehaviorTree>
  </root>)";

  TaskSpec spec;
  spec.task_type = "PRICE_CHECK";
  spec.slots = { {"user", "nebula"}, {"watchlist", {"AAPL", "TSLA", "NVDA"}} };

  auto task = engine.submitFromText(xml, spec);
  task.wait();
  std::cout << task.explain().dump(2) << "\n";
}
```

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/examples/hello_tree/hello_tree
```

C++17, no other runtime dependencies beyond BehaviorTree.CPP.

## Tree packs: trees are versioned assets

Business trees ship as **packs** — a directory with a manifest:

```json
{
  "name": "delivery", "version": "1.2.0",
  "templates": {
    "deliver": { "tree": "deliver_item",
                 "schema": { "slots": { "target_item": {"type":"string","required":true} } },
                 "timeout_ms": 60000 }
  },
  "task_types": {
    "DELIVER_ORDER":      { "template": "deliver" },
    "DELIVER_ORDER_FAST": { "template": "deliver", "slot_defaults": {"grasp_retry": 1} }
  }
}
```

```cpp
engine.loadPack("packs/delivery/1.2.0");  // load = validate every tree (CI gate)
auto task = engine.submitTask(spec);      // task_type -> pack -> tree
engine.setCanary("delivery", "1.3.0", 10); // 10% of new tasks try the canary
```

Load-time validation rejects broken trees before any task runs. Variants are
template instances with slot defaults — not forked XML. Canary routing is a
deterministic task-id hash; recovery replays pin the original version.

## CLI

```
nebula validate <tree.xml> [schema.json]   # load-time validation (CI gate)
nebula journal  <journal.jsonl>            # inspect persisted journals
nebula explain  <journal.jsonl> [task_id]  # post-mortem: timeline, checkpoints, outcome
nebula doc      <tree.xml>                 # tree -> mermaid flowchart
```

## Status

**1.0.0** — the core feature set is complete and regression-tested
(21 test cases, including a real fork + `SIGKILL` recovery test).
See the [roadmap](docs/DESIGN.md#roadmap) for what's next.

Explicit non-goals: not a BPMN / human-workflow engine, not a rules engine,
single-process only (journal design leaves the door open).

## Language

- [English](README.md)
- [简体中文](README.zh-CN.md)

## Credits

Built on top of [BehaviorTree.CPP](https://github.com/BehaviorTree/BehaviorTree.CPP)
by Davide Faconti — NebulaBT does not modify it and would not exist without it.

## License

Apache-2.0
