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

### 4.10 测试替身(M3)

`testkit::ScriptedAction`:按脚本返回(`then / thenFail / always`),
自动录制每次激活的输入端口快照(`calls()`,经 `NebulaCtx::inputsJson()`)。
与 RetryWithDelay/Checkpoint 组合覆盖金路径、失败兜底、恢复重跑——全部 CI 内完成。

### 4.9 `nebula` CLI(部分实现)

```
nebula validate <tree.xml> [schema.json]   # 加载期校验,CI 门禁(M1)
nebula journal  <file.jsonl>               # 持久化 journal 排查(M2)
nebula doc      <tree.xml>                 # 树 -> mermaid 流程图(M3)
```
规划:explain / replay。

## 5. 路线图 {#roadmap}

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M0 ✅** | 核心骨架:调度器 / SDK / Bridge / ForEach / 失败记录 / explain / 取消 | hello_tree 跑通;单测覆盖同步/异步/遍历/取消/定时器 |
| **M1 ✅** | 类型化 schema 校验(L1+L2)✅;策略(超时 ✅ / RetryWithDelay ✅ / 墙钟预算 ✅);失败自动捕获 ✅;`nebula validate` CLI ✅ | **试金石通过**:`examples/robot_delivery` 演示典型"拣选-递送"业务,搜寻段 N 份复制 → 1 份数据驱动,全树在 schema 严格模式下自洽 |
| **M2(核心已达成)** | journal 事件日志 ✅;Checkpoint 装饰器 ✅;断点恢复(含跨进程)✅;动态树原语补齐(WhileLoop/SwitchN)✅ | 恢复演示:递交失败重跑,已完成段整体跳过、副作用不重复 |
| **M2 ✅** | explain 完整版(节点 attempts / running_ms)✅;**kill -9 实机恢复测试** ✅(fork 子进程真杀 + journal 落盘 + 新进程恢复);`nebula journal` CLI ✅ | tests/kill9:检查点先落盘 → SIGKILL → 新进程恢复,副作用零重复 |
| **M3(核心已达成)** | 树包版本化(loadPack 加载即校验)✅;灰度路由 setCanary ✅;ScriptedAction 测试替身 ✅;`nebula doc` 自动流程图 ✅;robot_delivery 升级为 pack 工作流 ✅ | 20 组单测;pack 双版本灰度路由断言 |
| M3 收尾 | 模板树+实例覆写;`requires` 引用;explain/replay CLI;1.0 发布(README 定稿/贡献指南) | 社区发布清单 |

## 6. 已知限制

- explain 中节点状态为终态后会被 BT.CPP 重置为 IDLE,报告反映的是重置后状态(非运行中快照)
- 未提供 schema 时,slots 注入规则为 string / array→vector<string> / 其它 dump;
  提供后标量类型按真实类型注入
- L2 数据流检查不覆盖 Script/ScriptCondition 代码串中引用的键
- Checkpoint 快照支持 string/int/int64/double/bool/vector<string>,其它类型跳过;
  不做原子落盘(进程崩溃瞬间正在写的 journal 行可能不完整,恢复侧应容错解析)
- Engine 析构时未完成任务直接终止(halt 不会被调用),析构前应确保任务完成或取消
- `ForEachNode` 仅支持 string 集合;结构化元素待后续类型系统

## 7. 决策记录

所有重要设计决策见 [docs/adr/](adr/)。当前:

- [ADR-001](adr/ADR-001-bt-cpp-foundation-and-cpp17.md) 基于 BT.CPP 4.9 与 C++17,不重写树引擎
- [ADR-002](adr/ADR-002-callback-continuation-node-api.md) 回调续传节点 API
- [ADR-003](adr/ADR-003-single-json-source.md) 统一 JSON 实现(BT.CPP 内置)
