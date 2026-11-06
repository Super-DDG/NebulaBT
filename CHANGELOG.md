# Changelog

All notable changes to NebulaBT are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versioning is [SemVer](https://semver.org/).

## [Unreleased] - 代码审查修复批次(A1-A4 + B5/B6/B7)

### Fixed

- **A1(UB)**:explain() 的快照 job 按引用捕获栈上局部变量——超时返回后
  job 迟到执行(或引擎析构 drain 队列时)写悬垂引用。改为 shared_ptr
  共享状态安全交接。
- **A4**:explain() 的 post 被拒(队列满/引擎已停止)时不再白等 2s,
  立即降级返回 `tree_snapshot: unavailable`。
- **A2(死锁)**:`logf` 持 mu 锁调用用户 logger——logger 内重入任何
  Engine API 即死锁。改为先拷贝回调、锁外调用(与 journal_sink/
  on_finished 同款)。
- **A3(数据竞争)**:journal 帧格式 CRC32 查表为运行期惰性初始化,
  写侧/读侧首次并发调用存在竞态。改为编译期 constexpr 表。
- **B5(死锁)**:`advanceVirtualTime` 从调度线程自身调用(如动作内调
  `engine.advanceTime`)会在两阶段等待上无限自死锁。增加调度线程
  自调用防护,拒绝并告警。
- **B6(僵尸任务)**:提交的首次 tick 投递被拒(队列满/已停止)时,
  任务停在 Queued 永不执行却占用 active 槽位与资源名额。现在撤销
  注册并返回空句柄。
- **B7(静默数据丢失)**:单任务内存 journal 1000 条上限静默截断——
  长任务超限后新检查点不落内存,恢复时已完成段被重执行。上限改为
  `EngineLimits.max_journal_events`(0 = 不限),触顶告警一次;
  sink 落盘不受影响。
- 顺带:消除 4 处 `[[nodiscard]]` 编译告警,移除失效常量 kJournalMax。

每项修复均配回归测试(test_review_fixes)。

## [Unreleased] - M8 Agent 骨架

### Added

- **LLMDecision 节点**:`nodes/llm_decision.hpp` — LLM 只做"选分支",
  控制流仍在树里。LLM 客户端经能力接口注入(核心库零 HTTP 依赖);
  分支集合即 schema,输出不在集合内 → `LLM_INVALID_BRANCH`(幻觉被拦在
  节点边界);`__fail_code` 保留键约定:done 在任意线程执行时无法访问
  ctx,由桥在调度线程内代记结构化失败——异步包装器校验逻辑只依赖
  捕获副本,天然线程安全。
- **失败码登记表**:失败码是接口,先声明后使用。
  `Engine::declareFailureCodes()` / pack 清单 `task.failure_codes`;
  未登记的码在 FailureRecord 标记 `undeclared_code`(explain 可筛出
  拼写错误);登记表为空 = 不校验(向后兼容)。
- **game_npc 场景 E**:LLM 驱动决策三连——决策巡逻 / 决策交战完成 /
  **交战中玩家撤离 → ReactiveGuard 裁决否决 LLM 决策**(GUARD_VIOLATED,
  在途交战被 halt)。"LLM 提议,守护裁决"完整可跑;explain 输出
  决策 + 违规的审计链。
- **`nebula validate --json`**:机器可读输出,LLM 生成树草稿 → 校验 →
  错误回灌修复 → 人审入库的护栏闭环。

### Fixed

- **ReactiveGuard 回归**:子节点 RUNNING 且守护成立的路径漏掉了
  `child()->executeTick()` —— 异步动作的 done 结果永远无法被应用,
  任务永久卡 RUNNING。守护成立时照常 tick 子节点。

## [Unreleased] - M7 可观测与便携

### Added

- **Groot2 实时可视化**:`TaskSpec.groot2_port` — 任务运行期间向 Groot2
  GUI 实时推送全树节点状态(BT.CPP 自带 publisher,引擎零改动集成);
  端口占用仅告警不影响执行。
- **运行中 explain**:`explain()` 在任务 RUNNING 时安全可用——树遍历
  改为向调度线程投递快照 job(消除与 tick 并发读树的不安全窗口);
  终态后仍走直接遍历。排障恰好在任务卡住时最需要 explain。
- **树嵌入二进制**:`nebulabt_embed_trees()` CMake 函数(随安装包分发,
  `lib/cmake/nebulabt/nebulabt-embed.cmake`)— 构建期把 XML 转成字符串
  常量头,无文件系统的 MCU / 现场防树文件被改坏;`test_embed` 验证。
- **失败自动富化**:`FailureRecord.inputs` — `ctx.fail()` 自动附带激活
  时刻的输入端口快照({key} 已解析为黑板值),排障不用猜"当时参数"。
- 库尺寸基准:nebulabt 自身 ~1.26 MB text(Release,未 strip);
  全量组件化裁剪推迟到有真实 MCU 目标后再评估。

## [Unreleased] - M6 确定性内核与回放

### Added

- **时钟注入**:`Engine(std::shared_ptr<VirtualClock>)` — 超时 / 重试延时 /
  watchdog 静默计算 / 活动戳统一走 `NebulaClock`;`Engine::advanceTime(ms)`
  推进虚拟时间并把到期定时器**物化为调度作业**(单线程 tick 模型不变,
  两相推进:先在旧时刻排空既有作业,再触发到期定时器)。测试中重试 /
  超时 / 看门狗场景零真实等待。真实时钟引擎调用 advanceTime 仅告警。
- **故障注入 kit**:`testkit::flaky(action, fail_first, code, msg)` —
  注册层包装,生产动作代码零改动;计数跨 Engine 存活,可模拟故障
  横跨多轮恢复。
- **恢复性质测试 harness**(新测试二进制 `nebulabt_property`):200 轮
  随机推进 × 随机 done 完成时机 × 随机崩溃(引擎析构)× 恢复重跑,
  每轮断言"已记录检查点的副作用绝不再执行、未完成段至多一次、
  最终成功轮未完成段恰好一次";journal 走帧格式持久化(与生产路径
  完全一致)。kill9 的单点验证泛化为性质测试。
- **`nebula replay <journal-file> [--until N]` CLI**:恢复态预演——
  哪些检查点会被跳过、恢复哪些黑板键、上次运行终态;纯 journal 分析,
  不执行树。`nebula journal` / `explain` 同步支持帧格式自适应读取。

### Fixed

- watchdog 测试中 `RetryWithDelay` 端口名笔误(`num_attempts` →
  `attempts`)导致该用例此前空跑通过。

## [Unreleased] - M5 韧性原语

### Added

- **Compensable(Saga 补偿)**:双子控制节点 `[主工作, 补偿子树]`,主工作
  SUCCESS 即"武装";任务终态 FAILED 时引擎**自动逆序执行**全部已武装
  补偿(超时/路径同)——"扣了 A、B 失败,忘补偿 A"在结构上不可能发生。
  补偿失败记 `COMPENSATION_FAILED` 并继续逆序;journal 记
  compensation_armed/started/done。SUCCEEDED/CANCELED 不触发补偿。
- **ReactiveGuard(运行期守护)**:`while_true` 黑板 bool 键运行期巡检
  (调度器定时驱动,非业务轮询);破坏即 halt 子树 + 记
  `GUARD_VIOLATED` + 守护节点 FAILURE(父层走恢复分支)。键缺失/类型
  不符 **fail-closed**;适用安全区/e-stop/电量等"子树只能在条件成立时
  运行"的机器人刚需。
- **资源仲裁**:`TaskSpec.resources` 声明互斥资源,与活动任务声明集
  有交集 → 提交拒绝(终态自动释放)。单进程任务级互斥,防"两个任务
  同时抓同一包裹"。
- order_fulfillment 示例:手写补偿分支删除,改为 Compensable 声明式。

## [Unreleased] - M4 稳定地基

### Added

- **Journal 掉电安全**:帧格式持久化 `[u32 len][u32 crc32][payload]`
  (`JournalFileWriter`,可选每事件 fsync);容错读取 `readJournalFile` —
  截断尾帧 / CRC 不符时保留完好前缀并标记 `torn_tail`,不再整文件报废。
  kill -9 测试升级:落盘走帧格式,并注入撕裂尾帧验证恢复侧。
- **资源上限**:`Engine::setLimits(EngineLimits)` — 最大活动任务数
  (超限提交拒绝)、scheduler 队列与定时器上限(超限投递拒绝并告警),
  防失控回调风暴无界增长。
- **任务 watchdog**:`Engine::setWatchdog(interval, halt_on_trigger, cb)`
  — 任务 RUNNING 静默(无节点状态迁移)超限即告警,可选 halt +
  `TASK_WATCHDOG` 终态;`TaskSpec.watchdog_ms` 按任务覆盖;同一静默期
  只触发一次,活动恢复自动重新武装。

### Changed

- **engine.cpp 拆分**(1168 行 → 7 个文件):`engine_core` / `bridge_node` /
  `task_context` / `node_stats` / `tree_check` / `task` / `nebula_ctx`,
  为后续里程碑铺路。公共 API 无变化。

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
