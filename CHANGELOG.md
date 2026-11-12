# Changelog

All notable changes to NebulaBT are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versioning is [SemVer](https://semver.org/).

## [Unreleased]

### Fixed

- **失败记录在 gcc-12 下全部静默丢失**(CI 抓获,实参求值顺序未指定的
  UB):BridgeNode::initCtx 把 `findFailures(taskId)` 与
  `std::move(taskId)` 放在同一构造调用——两者求值顺序未指定,gcc-12 下
  move 先行,findFailures 收到空串,failures_ 为 null,所有 ctx.fail
  静默丢弃(执行不受影响,可观测性归零)。改为先取出再 move。
  同批:FetchContent 构建链接失败(BTCPP_GROOT_INTERFACE 裁剪,
  `NEBULABT_WITH_GROOT2` 选项化)、FetchContent tag 修正(4.9.0,
  无 v 前缀)、initCtx 读不到 task_id 时告警而非静默降级。

## [1.1.0] - 2026-11-06

M4–M8 五个里程碑 + 三轮质量收尾。核心叙事:**崩溃安全 → 保护自动包裹 →
生产可回放 → 设备可裁剪 → AI 可编排**,每一环有 CI 测试背书(11 个测试
程序,ASan/UBSan 绿,行覆盖 ~90%,性能基线已记录)。

### Added — 稳定地基(M4)

- **Journal 掉电安全**:帧格式持久化 `[u32 len][u32 crc32][payload]`
  (`JournalFileWriter`,可选每事件 fsync);容错读取 `readJournalFile` —
  截断尾帧 / CRC 不符时保留完好前缀并标记 `torn_tail`。
- **资源上限**:`Engine::setLimits(EngineLimits)` — 最大活动任务数、
  scheduler 队列与定时器上限、单任务内存 journal 上限(触顶告警,
  sink 落盘不受影响)。
- **任务 watchdog**:`Engine::setWatchdog(interval, halt_on_trigger, cb)`
  — RUNNING 静默超限即告警,可选 halt + `TASK_WATCHDOG`;
  `TaskSpec.watchdog_ms` 按任务覆盖;同一静默期只触发一次。

### Added — 韧性原语(M5)

- **Compensable(Saga 补偿)**:双子控制节点 `[主工作, 补偿子树]`,任务
  FAILED 时引擎**自动逆序执行**已武装补偿(超时路径同);补偿失败记
  `COMPENSATION_FAILED` 并继续;SUCCEEDED/CANCELED 不触发。
- **ReactiveGuard(运行期守护)**:`while_true` 黑板 bool 键持续巡检,
  破坏即 halt 子树 + `GUARD_VIOLATED`(fail-closed)。
- **资源仲裁**:`TaskSpec.resources` 声明互斥资源,冲突提交拒绝,
  终态自动释放。

### Added — 确定性内核与回放(M6)

- **时钟注入**:`Engine(shared_ptr<VirtualClock>)`;超时/重试延时/watchdog
  统一走 `NebulaClock`;`Engine::advanceTime(ms)` 把到期定时器物化为调度
  作业(单线程 tick 模型不变)。测试零真实等待。
- **故障注入**:`testkit::flaky(action, fail_first, code, msg)` — 注册层
  包装,生产代码零改动,计数跨 Engine 存活。
- **恢复性质测试**(`nebulabt_property`):200 轮随机推进 × 随机完成时机 ×
  随机崩溃 × 恢复重跑,断言副作用恰好一次。
- **`nebula replay` CLI**:恢复态预演(跳过哪些检查点/恢复哪些键);
  journal/explain 同步支持帧格式自适应读取。

### Added — 可观测与便携(M7)

- **Groot2 实时可视化**(`TaskSpec.groot2_port`)。
- **运行中 explain**:RUNNING 时经调度线程快照安全可用。
- **树嵌入二进制**:`nebulabt_embed_trees()` CMake 函数,无文件系统可用。
- **失败自动富化**:`FailureRecord.inputs` 附带激活时刻输入端口快照。

### Added — Agent 骨架(M8)

- **LLMDecision 节点**:LLM 只做"选分支",控制流仍在树里;客户端经
  能力接口注入(核心零 HTTP 依赖);幻觉分支 → `LLM_INVALID_BRANCH`;
  `__fail_code` 保留键约定支持任意线程结构化失败。
- **失败码登记表**:`Engine::declareFailureCodes()` / pack 清单声明;
  未登记码标记 `undeclared_code`;表为空 = 不校验。
- **`nebula validate --json`**:LLM 生成 → 校验 → 修复 → 人审的护栏闭环。
- game_npc 场景 E:"LLM 提议,守护裁决"完整可跑。

### Added — 零编译宿主与可选 YAML 清单

- **pack 清单双格式**:pack.json 保持规范(零依赖不变);
  `-DNEBULABT_WITH_YAML=ON`(yaml-cpp)后 `pack.yaml` 完全等价。
- **零编译宿主**(`examples/zero_compile`):通用原语(Log 支持 `{key}`
  文案插值 / Wait / SetSlot / CheckSlot / Fail)+ REPL(`list` / 提交 /
  `load <dir>`);新任务 = 丢一个 pack 目录,运行时生效,CTest 常驻。
- **SDK**:`ctx.inRef(port)` / `ctx.peek(key)`(动态按键访问);
  `Engine::loadedTaskTypes()`。

### Added — 工程化

- **CI**(GitHub Actions):编译器矩阵、ASan/UBSan job、YAML 清单 job、
  覆盖率报告(artifact + job summary)。
- **消毒器构建**:`NEBULABT_SANITIZE=address|undefined|thread`。
- **性能基准**(`nebulabt_bench`,基线见 docs/benchmarks.md):单次 tick
  ~630ns、任务周转 ~150µs、journal 帧落盘 ~2.5µs/事件。
- **CLI 冒烟 / 并发压测 / 边界单测**:新增 `nebulabt_cli`、`nebulabt_stress`
  及边界用例组;覆盖率报告 docs/coverage.md(~90%)。

### Changed

- engine.cpp 拆分(1168 行 → 7 个文件),公共 API 无变化。
- `TaskHandle::waitFor` 改原子轮询(1ms 粒度;GCC 11 TSan 对 timed cv
  建模不全的误报链随之消除);`wait()` 保留 cv 阻塞。

### Fixed

- **EngineCore 引用环泄漏**(ASan):factory 持有的 wake lambda 捕获
  `shared_ptr<EngineCore>` 自环;改 `weak_ptr` + 外联析构。
- **树创建并发竞争**(并发压测;sanitizer 因库未插桩不可见):BT.CPP
  factory 共享 XMLParser,Engine 以 `create_mu` 串行化创建点。
- **finished 事件竞态**(ASan):终态 journal 事件在唤醒 wait() 之后才落,
  持久化可丢终态记录;顺序改为 补偿 → finished → 唤醒。
- **explain 快照 job 悬垂引用**(UB):改 shared_ptr 共享状态;post 被拒
  立即降级。
- **logf 持锁回调死锁**:用户 logger 重入 Engine API 即死锁;改锁外调用。
- **journal CRC32 惰性表初始化数据竞争**:改编译期 constexpr 表。
- **僵尸任务**:首次 tick 投递被拒时撤销注册,不再占用 active/资源槽位。
- **tick 异常只进日志**:现记 `TICK_EXCEPTION` 结构化失败。
- **pack 失败码登记于 move 之后**(moved-from 为空,从未注册):
  提前到 move 前,补声明/未声明码回归断言。
- **ReactiveGuard 回归**:守护成立路径漏 tick 子节点,异步 done 永不
  应用、任务永久卡 RUNNING。
- **advanceVirtualTime 调度线程自调用自死锁**:拒绝并告警。

## [1.0.0] - 2026-11-05

First tagged release. Core feature set complete and covered by 21 test cases
(2 binaries, incl. a real `SIGKILL` recovery test).

### Added

- **Execution model**: event-driven scheduler (wake on completion callbacks /
  timers / external commands; zero polling); callback-continuation node SDK
  (`NebulaAction` / `NebulaCtx` / `Done`), C++17, built on BehaviorTree.CPP 4.9.
- **Tasks**: lifecycle (submit / wait / cancel / onFinished), typed blackboard
  slots injection, task-level timeout policy (`TASK_TIMEOUT`).
- **Failure as data**: structured `FailureRecord`s, auto-capture of silent
  node failures (`NODE_FAILURE`), `explain()` with per-node
  attempts / running_ms / first_ms / last_ms via full-tree status tracing.
- **Dynamic tree primitives**: `ForEach`, `WhileLoop`, `SwitchN` (unbounded
  cases), `RetryWithDelay` (inter-attempt delay + wall-clock budget).
- **Journal & recovery**: per-task event journal (sink for persistence),
  `Checkpoint` decorator with full-blackboard snapshots, resume via
  `TaskSpec.resume_journal` — completed checkpoints skip their subtree and
  restore state; committed side effects never re-execute. Verified by a
  fork + SIGKILL test.
- **Tree packs**: directory-based versioned assets (`pack.json` + trees);
  load-time validation of every tree (parse + schema dataflow); templates
  with slot-defaults (variants without forks); `requires` dependency checks;
  canary routing by deterministic task-id hash with explicit version pinning.
- **Testing**: `testkit::ScriptedAction` mock with input recording.
- **CLI**: `nebula validate` (CI gate), `nebula journal`, `nebula explain`,
  `nebula doc` (tree → mermaid flowchart).

## [0.1.0] - 2026-11-04

Initial skeleton: scheduler, SDK, bridge, ForEach, failure records, explain,
task lifecycle.
