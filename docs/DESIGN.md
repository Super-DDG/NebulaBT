# NebulaBT 设计文档

> 星云,是宇宙中最混沌的物质;它坍缩,才成为恒星。
> 业务逻辑也是一团星云——NebulaBT 给它们引力,让混沌坍缩成一棵清晰的树。

## 1. 定位

NebulaBT 是一个面向业务开发的行为树编排框架,目标行业:机器人、游戏、后端服务。

**它是**:进程内业务逻辑的组织方式——声明式任务协议、结构化失败、事件驱动执行、断点恢复、树级测试。

**它不是**(同样重要):

- 不是 BPMN / 人工审批工作流引擎
- 不是规则引擎
- v1 不做分布式编排(journal 设计为其留门)
- 不是 BT.CPP 的替代品,而是它之上的应用层

## 2. 痛点 → 机制映射

| 业务工程痛点 | 传统项目的下场 | NebulaBT 机制 |
|---|---|---|
| 需求频繁变更 | 改一处动全身,最后没人敢动 | 树包版本化;运行中实例钉住旧版本 |
| 灰度发布 | 全量 or 不发 | 任务级路由到不同树版本 |
| 长链路排障 | 翻几小时日志还原路径 | explain API + OTel |
| 故障恢复 | 状态全丢,整单重跑 | journal + checkpoint 恢复 |
| 回归测试 | 三个场景手测,其余听天由命 | golden test + 生产 journal 回放 |
| 多人协作 | if-else/XML 合并冲突 | 子树文件化 + 树包引用 |
| 新人接手 | "这个判断为什么存在"无人能答 | 节点 description 元数据 → 自动流程文档 |
| 现场调参 | 改代码发版 | 策略参数与树定义分离,配置注入 |
| 集合遍历 | 静态树被迫复制粘贴 | ForEach 动态展开原语 |

## 3. 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│ 行业接入层   ROS2 │ 游戏引擎 │ HTTP / gRPC / DB                │
├─────────────────────────────────────────────────────────────┤
│ 能力层       NebulaAction 节点 + Mock 生成(规划)              │
├─────────────────────────────────────────────────────────────┤
│ NebulaBT核心  Scheduler(事件驱动调度)                        │
│              Lifecycle(任务状态机)                           │
│              Policy(超时/重试/抢占策略,规划)                  │
│              Failure(结构化失败传播)                          │
│              Journal(事件溯源与恢复,规划)                     │
│              Registry(树包与版本,规划)                        │
├─────────────────────────────────────────────────────────────┤
│ 树引擎       BehaviorTree.CPP v4(包依赖,不修改源码)           │
└─────────────────────────────────────────────────────────────┘
```

## 4. 核心子系统

### 4.1 执行模型:事件驱动调度器(已实现)

框架中**唯一的 tick 驱动源**。无事件时线程休眠,不做任何轮询。唤醒源四类:

- adapter 完成回调(`Done`)
- 定时器(`Scheduler::callLater`)
- 外部命令(submit / cancel)
- 黑板键变更(M1)

业务节点的异步由回调续传表达(见 ADR-002):`start()` 发起调用返回 RUNNING,完成时在**任意线程**调用 `done(status, result)`,调度器被唤醒、树继续推进。

### 4.2 结构化失败(已实现)

失败是数据,不是日志行。约定:

- 节点内 `ctx.fail(code, message)` 记录,自动附带节点名/注册名/时间戳
- **失败自动捕获**:动作返回 FAILURE 却没调用 `ctx.fail()` 时,框架兜底记录
  `NODE_FAILURE`——失败可解释不依赖作者自觉
- 任务终态可查询:`task.failures()` 返回结构化记录数组
- `task.explain()` 返回完整执行报告:树形结构 + 各节点状态 + 失败记录 +
  **节点计时**(attempts / running_ms,经 BT.CPP StatusChangeLogger 订阅全树迁移)

规划:失败码登记表。

### 4.3 动态树原语(已实现:ForEach)

行为树的 XML 是静态的,"对购物车每个商品做校验"这类按数据集合展开的逻辑,一直是 BT 进入业务领域的最大阻力——静态树的解法只有复制粘贴,同一段"导航+检测"子树按地点复制多份是真实项目中的常见反模式。

`ForEachNode`:遍历 `array`(黑板 `vector<string>` 或 `;` 分隔字符串),每轮将当前元素写入 `element` 输出端口,依次 tick 子节点。子节点 RUNNING → 整体 RUNNING(下轮续跑);FAILURE → 默认整体失败,`continue_on_failure="true"` 时跳过。

**WhileLoop**:真正的 while 循环(condition + body,非 reactive)。语义:body RUNNING 期间不重估 condition;body SUCCESS → 重估 condition 决定继续/正常退出(SUCCESS);body FAILURE → 整体失败;condition FAILURE → 正常退出。与轮询式运行时的关键适配:body SUCCESS 后**同 tick 重估 condition** 并把子节点 reset 回 IDLE(轮询式实现靠外层下一 tick 推进,事件驱动模型没有下一 tick 就死等);`max_iterations`(默认 1000)防全同步死循环。

**SwitchN**:可变 case 数的 Switch(`case_1..case_32`,default 恒为最后一个子节点)。BT.CPP 4.9 内置 Switch2~6 模板仍有 case 上限;SwitchN 语义与其一致(切换分支时 halt 上一个 RUNNING 子节点),去掉数量限制。

> 实现教训(WhileLoop):BT.CPP 的 `StatefulActionNode` 处于终态时再次
> `executeTick()` 会**直接返回缓存的终态**而不调 onStart——循环类控制节点
> 在重入子节点前必须先 reset,否则静默空转。

规划(M1+):运行时按数据实例化子节点、WhileUntil、ParallelMap。

### 4.4 任务生命周期(已实现,M0 基础版)

```
QUEUED → RUNNING → SUCCEEDED / FAILED / CANCELED
```

- 提交:slots 注入黑板(string 直存、array→vector<string>、其余 dump)
- 取消:`cancel()` → halt 整棵树 → 各动作 `halt()` 回调 → CANCELED
- 终态回调:`Engine::onFinished`(调度线程执行)

### 4.5 类型化黑板与校验(已实现,M1)

加载期校验,目标:**错误在加载期爆,不在深夜爆**。

- **L1 提交 ↔ schema**:required 缺失 / 未知字段(防拼写错)/ 类型不符,
  提交时拒绝;schema 声明的 int/double/bool 按**真实类型**注入黑板
- **L2 树 ↔ schema(端口级数据流检查)**:树中端口引用的每个黑板键,
  必须是 schema 声明键、框架注入键(task_id/task_type)或树内某节点
  Output 写出的键——键名拼写错误、"注入了但没人读"在加载期暴露
- schema 未提供 = 关闭校验,完全向后兼容
- `nebula validate <tree.xml> [schema.json]` CLI:CI 门禁
- 树内端口类型检查由 BT.CPP 加载期完成(免费)

已知边界(L2 v0):Script/ScriptCondition 代码串中引用的键暂不检查;
后续随树包(M3)补齐。

### 4.6 策略引擎(已实现,M1)

- **任务级超时**:`TaskSpec.timeout_ms`,到点未终态则 halt 整棵树 + 记录
  `TASK_TIMEOUT` + 终态 FAILED
- **RetryWithDelay 装饰器**:BT.CPP 4.9 内置重试无尝试间延时;NebulaBT 提供
  带延时的版本,延时到期经调度器唤醒,无轮询
- **重试墙钟预算**:`budget_ms` 端口,次数与时间任一用尽即失败
  ——防"30 次重试跑几十分钟"

规划:抢占规则;由框架自动包裹(作者忘不掉的保险)。

### 4.7 持久化与恢复(已实现,M2 核心)

BT.CPP 无法快照"哪个节点正在 RUNNING"(那是 C++ 调用栈状态)。务实路线:

- **Journal 事件日志**(✅):任务生命周期事件(submitted/started/checkpoint/
  checkpoint_restored/finished)记录在任务上并可经 sink 回调落盘——观测与恢复一鱼两吃。
  `TaskHandle::journal()` 取快照;`Engine::setJournalSink()` 持久化(每事件一行 JSON)。
- **Checkpoint 装饰器**(✅):子节点 SUCCESS 时捕获**全量黑板快照**(排除框架键,
  支持 string/int/int64/double/bool/vector<string>)记入 journal
- **断点恢复**(✅):`TaskSpec.resume_journal` = 上一轮 journal(内存传递或落盘后跨进程读回),
  提交同一棵树:已完成的 Checkpoint **跳过子节点**、恢复黑板快照——
  上一轮已提交的副作用不重复执行(业务节点需幂等,见反目标)
- 恢复语义演示:`examples/robot_delivery` 场景 C——递交失败重跑,搜寻/抓取整体跳过

不做"树中间任意位置断点续跑"。规划:checkpoint 的原子落盘(配 WAL)、
resume 时的树版本一致性校验。

### 4.8 树包与版本(已实现,M3 核心)

树是带版本的业务资产。pack = 目录(pack.json 清单 + trees/*.xml):

- **loadPack(目录)**:加载即全量校验——每棵树可解析、schema 数据流检查通过,
  失败抛异常;CI 中加载全部 pack 就是最强门禁
- **submitTask(spec)**:按 task_type 从 pack 解析树文件;schema/timeout_ms
  由 pack 声明,业务代码不再重复;explain/journal 记录来源 pack 版本
- **灰度路由 setCanary(pack, version, percent)**:task_id 哈希确定性路由,
  percent 命中走 canary 版本,其余走最高版本;`TaskSpec.pack_version`
  显式钉住(恢复重跑必须钉住原版本)
- 同名同版本重复加载抛异常;模板树+实例覆写与 `requires` 引用为后续演进

### 4.9 确定性内核(M6,已实现)

- **时钟注入**:`NebulaClock` 抽象 + `VirtualClock`(手动 advance)。超时 /
  RetryWithDelay 延时 / watchdog / 活动戳统一走引擎时钟。`advanceTime(ms)`
  两相推进:先在旧虚拟时刻排空已排队作业(submit 的首次 tick 不会被打上
  未来时间戳),再把到期定时器按(到期时间,注册序)物化为调度作业并
  等其级联执行完——advance 返回后引擎状态确定,测试零真实等待。
  定时器仍由调度线程串行执行,单线程 tick 模型不变。
- **故障注入**:`testkit::flaky(action, fail_first, code)` 注册层包装,
  生产代码零改动;与虚拟时钟/Checkpoint 组合可复现任意恢复交错。
- **恢复性质测试**:`test_property_recovery` —— 200 轮随机推进 × 随机
  崩溃 × 恢复,断言"已记录检查点的副作用绝不再执行"(kill9 泛化)。
- **`nebula replay`**:恢复态预演(跳过的检查点 / 恢复的黑板键 /
  上次终态),纯 journal 分析不执行树;`--until N` 为时间旅行点。

### 4.10 韧性原语(M5,已实现)

业务反复手写的保护逻辑,做成引擎原语(保险由框架包裹,不靠作者自觉):

- **Compensable(Saga 补偿)**:双子控制节点,child[0] 主工作、child[1]
  补偿子树。主工作 SUCCESS 即武装(sagaPush + journal
  compensation_armed);任务终态 FAILED 时引擎在唤醒 wait() **之前**
  按武装逆序自动执行补偿(超时/看门狗路径同),补偿失败记
  COMPENSATION_FAILED 并继续。SUCCEEDED/CANCELED 不触发补偿
  (取消语义由业务在 halt() 里决定)。补偿同步执行(调度线程),
  单条最多 tick 1000 次(防补偿内死循环)。
- **ReactiveGuard(运行期守护)**:`while_true` 黑板 bool 键,进入前与
  子树 RUNNING 期间每 period_ms 毫秒检查一次(调度器定时驱动 reactive
  重 tick,RUNNING 中的动作返回缓存状态不会重跑)。违反 → halt 子树 +
  GUARD_VIOLATED + 守护节点 FAILURE。键缺失/类型不符 fail-closed;
  无 schema 提交时 bool slots 以字符串落黑板,兼容 "true"/"false" 形态。
  while_true 必须声明为**无类型端口**:若声明为 InputPort<std::string>,
  BT.CPP 会把绑定的黑板键钉成 string,与 bool 写入冲突。
- **资源仲裁**:`TaskSpec.resources` 声明互斥资源;与活动任务声明集
  有交集 → 提交拒绝,任务终态自动释放。单进程 map 实现,分布式留门。

### 4.11 可观测与便携(M7,已实现)

- **Groot2 实时可视化**:`TaskSpec.groot2_port` 经 BT.CPP 自带
  Groot2Publisher 推送全树实时状态;publisher 生命周期与树一致,
  构造失败(端口占用)只告警。每个运行中任务需独立端口。
- **运行中 explain**:树遍历无锁,与 tick 并发读不安全——RUNNING 中
  向调度线程投递快照 job(在 tick 线程内遍历),终态后直接遍历。
  上限 2s:从调度线程自身调用会超时返回无树快照(文档明确)。
- **树嵌入二进制**:`nebulabt_embed_trees(<target> NAME path...)`
  配置期生成字符串常量头(`nebulabt_embed::NEBULA_TREE_<NAME>`),
  无文件系统 MCU 可用、现场树文件不可被改坏;安装于
  `lib/cmake/nebulabt/nebulabt-embed.cmake`。
- **失败富化**:`FailureRecord.inputs` 由 `ctx.fail()` 自动填充
  (激活时刻的输入端口快照)。
- **尺寸基准**:nebulabt 自身 ~1.26 MB text(Release)。全量组件化
  裁剪(#ifdef 拆 journal/checkpoint/pack)推迟:churn 大而当前无
  MCU 硬目标;等真实目标出现按其预算裁剪。

### 4.12 Agent 骨架(M8,已实现)

- **LLMDecision**:LLM 只做"选分支",控制流仍在树里。客户端经能力
  接口注入(`LlmDecision::Client`,核心库零 HTTP 依赖);分支集合即
  schema,越界输出 → `LLM_INVALID_BRANCH`。配套 SDK 保留键约定:
  done 的 result 携带 `__fail_code`/`__fail_message` 时由桥在调度线程
  代记结构化失败——任意线程回调的校验逻辑零 ctx 访问。
- **失败码登记表**:失败码是接口,先声明后使用(Engine / pack 清单
  `failure_codes`);未登记码运行时标记 `undeclared_code`,explain 可筛。
  登记表为空 = 不校验,向后兼容。
- **"LLM 提议,守护裁决"**:LLMDecision 选分支 + ReactiveGuard 包住
  执行段——决策可信的前提是守护条件持续成立(game_npc 场景 E3:
  交战中玩家撤离,守护否决决策、halt 在途交战)。
- **validate 护栏闭环**:`nebula validate --json` 机器可读输出,
  LLM 生成树草稿 → 校验 → 错误回灌修复 → 人审入库。
- **实现教训(v4 重 tick 语义)**:`Sequence` 是记忆式(已完成子节点
  不重跑),`ReactiveSequence/ReactiveFallback` 每 tick 从头重估并 halt
  其他分支——LLM 决策放记忆式段(只决策一次),感知刷新放反应式段
  (守护读到的黑板恒新鲜)。守护成立路径必须照常 tick 子节点,否则
  异步 done 结果无法应用(回归见 game_npc E2)。

### 4.13 测试替身(M3)

`testkit::ScriptedAction`:按脚本返回(`then / thenFail / always`),
自动录制每次激活的输入端口快照(`calls()`,经 `NebulaCtx::inputsJson()`)。
与 RetryWithDelay/Checkpoint 组合覆盖金路径、失败兜底、恢复重跑——全部 CI 内完成。

### 4.12 `nebula` CLI(已实现)

```
nebula validate <tree.xml> [schema.json]   # 加载期校验,CI 门禁(M1)
nebula journal  <file>                     # journal 排查(帧格式/JSONL 自适应)
nebula explain  <journal-file> [task_id]   # 执行轨迹还原(M2)
nebula replay   <journal-file> [--until N] # 恢复态预演(M6)
nebula doc      <tree.xml>                 # 树 -> mermaid 流程图(M3)
```

## 5. 路线图 {#roadmap}

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M0 ✅** | 核心骨架:调度器 / SDK / Bridge / ForEach / 失败记录 / explain / 取消 | hello_tree 跑通;单测覆盖同步/异步/遍历/取消/定时器 |
| **M1 ✅** | 类型化 schema 校验(L1+L2)✅;策略(超时 ✅ / RetryWithDelay ✅ / 墙钟预算 ✅);失败自动捕获 ✅;`nebula validate` CLI ✅ | **试金石通过**:`examples/robot_delivery` 演示典型"拣选-递送"业务,搜寻段 N 份复制 → 1 份数据驱动,全树在 schema 严格模式下自洽 |
| **M2(核心已达成)** | journal 事件日志 ✅;Checkpoint 装饰器 ✅;断点恢复(含跨进程)✅;动态树原语补齐(WhileLoop/SwitchN)✅ | 恢复演示:递交失败重跑,已完成段整体跳过、副作用不重复 |
| **M2 ✅** | explain 完整版(节点 attempts / running_ms)✅;**kill -9 实机恢复测试** ✅(fork 子进程真杀 + journal 落盘 + 新进程恢复);`nebula journal` CLI ✅ | tests/kill9:检查点先落盘 → SIGKILL → 新进程恢复,副作用零重复 |
| **M3(核心已达成)** | 树包版本化(loadPack 加载即校验)✅;灰度路由 setCanary ✅;ScriptedAction 测试替身 ✅;`nebula doc` 自动流程图 ✅;robot_delivery 升级为 pack 工作流 ✅ | 20 组单测;pack 双版本灰度路由断言 |
| M3 收尾 | 模板树+实例覆写;`requires` 引用;explain/replay CLI;1.0 发布(README 定稿/贡献指南) | 社区发布清单 |
| **M4 ✅** | **稳定地基**:journal 帧格式落盘 + CRC + 撕裂容错 ✅;资源上限(活动任务/队列/定时器)✅;任务 watchdog 卡死检测 ✅;engine.cpp 拆分 ✅ | 掉电注入(半截帧/CRC 坏帧)恢复前缀完好;超限拒绝有测试;watchdog 触发/不误报有测试 |
| **M5 ✅** | **韧性原语**:Compensable Saga 补偿 ✅;ReactiveGuard 运行期守护 ✅;资源仲裁 ✅;order_fulfillment 声明式补偿 ✅ | 补偿逆序/超时补偿/取消不补偿有测试;守护破坏 halt + fail-closed 有测试;资源冲突拒绝/终态释放有测试 |
| **M6 ✅** | **确定性内核**:时钟注入(VirtualClock,advanceTime 两相推进)✅;故障注入 kit(testkit::flaky)✅;恢复性质测试 harness(200 轮随机交错)✅;`nebula replay` CLI ✅;journal/explain CLI 帧格式自适应 ✅ | 重试/超时/watchdog 虚拟时间零等待;性质测试断言已记录检查点副作用绝不再执行;replay 预演与引擎恢复语义一致 |
| **M7 ✅** | **可观测与便携**:Groot2 实时可视化 ✅;运行中 explain(调度线程快照)✅;树嵌入二进制(nebulabt_embed_trees)✅;失败自动富化 ✅;尺寸基准 ~1.26 MB text | RUNNING 中 explain 有并发测试;嵌入树提交等同文件提交;groot2 端口占用不炸 |
| **M8 ✅** | **Agent 骨架**:LLMDecision 节点(能力接口注入,分支即 schema)✅;失败码登记表(Engine/pack 声明,undeclared_code 标记)✅;game_npc 场景 E(LLM 提议,守护裁决)✅;validate --json 护栏闭环 ✅ | 幻觉分支拦截有测试;未登记失败码可筛;守护否决 LLM 决策可跑可断言 |

## 6. 已知限制

- **BT.CPP 树创建非并发安全**:factory 内部持有共享 XMLParser,
  同一 Engine 的并发 submit/validate 由 `create_mu` 串行化(压测实证;
  sanitizer 因库未插桩而不可见)。不同 Engine 实例之间无共享,天然并行。
- **TSan(GCC 11 libtsan)在本工具链不可用作门禁**:对
  `condition_variable::wait_for`(timed)建模不全,mutex + timed cv 的
  标准库最小复现即产生 double-lock/race 级联误报;框架代码的 TSan
  报告已逐一溯源为该误报的下游。升级 GCC 12+/clang 后应复测。
- ASan 泄漏检测中,BT.CPP/tinyxml2 未插桩库内部分配可能以 indirect
  leak 形态出现;以"直接泄漏是否指向本仓库代码"为准。

- explain 中节点状态为终态后会被 BT.CPP 重置为 IDLE,报告反映的是重置后状态(非运行中快照)
- explain(M7 后)在任务 RUNNING 时安全可用:树遍历经调度线程快照 job。
  唯一限制:不要**从调度线程自身**(如 onFinished 之外的调度上下文)调用
  ——会等到 2s 超时并返回无树快照;终态后调用走直接遍历,无此限制
- 未提供 schema 时,slots 注入规则为 string / array→vector<string> / 其它 dump;
  提供后标量类型按真实类型注入
- L2 数据流检查不覆盖 Script/ScriptCondition 代码串中引用的键
- Checkpoint 快照支持 string/int/int64/double/bool/vector<string>,其它类型跳过
- journal 落盘推荐走 `JournalFileWriter` 帧格式(长度前缀 + CRC32,
  见 `core/journal_file.hpp`):崩溃瞬间最多损坏最后一帧,读取侧容错
  截断到完好前缀并标记 `torn_tail`;裸 JSONL sink(逐行 `dump()`)仍可用,
  但掉电窗口内的半行只能靠解析容错
- FailureRecord.attempt 暂未由 RetryWithDelay 自动填充(记录粒度到节点级)
- `ForEachNode` 仅支持 string 集合;结构化元素待后续类型系统
- 补偿子树内不要再嵌 Compensable(嵌套补偿在 runCompensations 弹栈后
  入栈,条目无人消费;Saga 一层语义通常足够)
- `NebulaCtx` 非线程安全:仅在 start/halt 调用栈内使用;跨线程失败
  经 Done 的 `__fail_code` 保留键由桥代记
- 单任务内存 journal 上限 `EngineLimits.max_journal_events`(默认 1000,
  0 = 不限):触顶只影响 explain 内存视图,sink 落盘不受影响,但恢复
  依赖落盘 journal;触顶会告警一次

## 7. 代码组织规范

### 目录结构

```
include/nebulabt/        公共头文件(随库安装,唯一对外接口面)
  core/                  引擎核心 API(engine/task/scheduler/pack/
                         journal/journal_file/failure/clock/schema)
  sdk/                   节点作者 SDK(action.hpp:NebulaAction/NebulaCtx/Done)
  nodes/                 内置节点(装饰器/控制节点/LLM 决策)
  testkit/               测试工具(mock/inject;仅测试用,不随安装)
src/                     实现(与 include 一一对应)
  core/                  内部头(engine_core/task_context/bridge_node/
                         node_stats/tree_check)不对外安装
src/nodes/               节点实现
tools/                   nebula CLI(validate/journal/replay/explain/doc)
tests/                   测试(单测/压测/CLI 冒烟/性质/kill9)
examples/                示例即测试
benchmarks/              性能基准(docs/benchmarks.md 记录基线)
scripts/                 辅助脚本(coverage.py 等)
docs/                    设计文档 + adr/
```

### 头/源分离规则

- **声明进 .hpp,实现进 .cpp**——公共头里只允许声明、文档注释与默认值。
  内部头(`src/core/*.hpp`)同规则。
- **豁免**(仅限以下形态,新增豁免需在此登记):
  - 模板:`sdk/action.hpp` 的 `in<T>/out<T>`、`core/any_json.hpp` 的
    `anyToJson`——模板必须在头文件实例化
  - 琐碎内联:`core/clock.hpp` 的时钟单行读/推进
  - 纯测试便利:`testkit/*.hpp` header-only(单头即用是测试库惯例,
    不随库安装)
- 全部头文件使用 `#pragma once`;节点注册名与文件名一一对应。

## 8. 决策记录

所有重要设计决策见 [docs/adr/](adr/)。当前:

- [ADR-001](adr/ADR-001-bt-cpp-foundation-and-cpp17.md) 基于 BT.CPP 4.9 与 C++17,不重写树引擎
- [ADR-002](adr/ADR-002-callback-continuation-node-api.md) 回调续传节点 API
- [ADR-003](adr/ADR-003-single-json-source.md) 统一 JSON 实现(BT.CPP 内置)
