# NebulaBT 使用文档

> 适用版本:v1.1.0 · 面向读者:使用 NebulaBT 组织业务逻辑的工程师
> 平台:Linux / POSIX(journal fsync 与时间 API 使用 POSIX 接口,Windows 暂不支持)
> 设计动机与架构见 [DESIGN.md](DESIGN.md);关键决策见 [docs/adr/](adr/)。

---

## 目录

1. [核心概念](#1-核心概念)
2. [安装与构建](#2-安装与构建)
3. [快速上手](#3-快速上手)
4. [编写业务节点(NebulaAction)](#4-编写业务节点)
5. [编写行为树(XML)](#5-编写行为树)
6. [提交任务](#6-提交任务)
7. [Schema:让错误在加载期爆](#7-schema-让错误在加载期爆)
8. [观测与排障](#8-观测与排障)
9. [断点恢复](#9-断点恢复)
10. [树包(Tree Pack)与灰度](#10-树包与灰度)
11. [树级测试(ScriptedAction)](#11-树级测试)
12. [内置节点参考](#12-内置节点参考)
13. [线程模型](#13-线程模型)
14. [错误码约定](#14-错误码约定)
15. [最佳实践与反模式](#15-最佳实践与反模式)
16. [Agent 骨架:LLM 提议,守护裁决](#16-agent-骨架llm-提议守护裁决)
17. [0 编译新增任务:通用宿主](#17-0-编译新增任务通用宿主)

---

## 1. 核心概念

| 概念 | 是什么 | 类比 |
|---|---|---|
| **行为树(XML)** | 业务流程的声明式描述;节点返回 SUCCESS/FAILURE/RUNNING | "业务流程图"本身 |
| **NebulaAction** | 你写的业务节点:一段异步/同步业务逻辑 | 流程图里的一个步骤 |
| **Engine** | 引擎:注册节点、加载树包、提交任务、事件驱动调度 | 运行时 |
| **Task** | 一次任务执行:一棵树 + 一份黑板;有生命周期 | 一张订单的处理过程 |
| **Blackboard(黑板)** | 任务级共享数据;端口通过 `{key}` 读写 | 订单上下文 |
| **Journal** | 任务事件日志(提交/启动/检查点/终态);观测与恢复的数据源 | 流水 |
| **Checkpoint** | 断点恢复原语:子树完成后拍黑板快照入 journal | 业务安全点 |
| **Tree Pack** | 树 + schema + 默认策略的带版本打包资产 | 一个服务版本 |
| **explain()** | 整棵树的执行报告:每节点状态/耗时/尝试次数/失败原因 | "这单为什么走了这条路" |

一句话心智模型:**你写"一段段业务逻辑"(节点)和"一张流程图"(树),引擎负责驱动、超时、取消、记录、恢复、灰度。**

---

## 2. 安装与构建

### 依赖

- C++17 编译器(GCC ≥ 9 / Clang ≥ 8;**仅 Linux/POSIX**,Windows 暂不支持)
- CMake ≥ 3.16
- BehaviorTree.CPP 4.9(自动 FetchContent,或系统已装则直接使用)
- (可选)yaml-cpp 0.7+,启用 `NEBULABT_WITH_YAML` 时需要

### 集成到你的项目

方式一:`add_subdirectory` / FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(nebulabt
  GIT_REPOSITORY https://github.com/Super-DDG/NebulaBT.git
  GIT_TAG v1.1.0)
FetchContent_MakeAvailable(nebulabt)

target_link_libraries(your_app PRIVATE nebulabt::nebulabt)
```

方式二:安装后 `find_package`

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --install build --prefix /opt/nebulabt
```

```cmake
find_package(nebulabt 1.1 REQUIRED)   # -DCMAKE_PREFIX_PATH=/opt/nebulabt
target_link_libraries(your_app PRIVATE nebulabt::nebulabt)
```

### 构建本项目自身

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure
```

可选开关:`NEBULABT_BUILD_EXAMPLES` / `NEBULABT_BUILD_TESTS` /
`NEBULABT_BUILD_TOOLS` / `NEBULABT_BUILD_BENCH`(默认 ON);
`NEBULABT_BT_CPP_SOURCE_DIR` 指向本地 BehaviorTree.CPP 源码(离线构建);
`NEBULABT_SANITIZE=address|undefined|thread` 出消毒器构建(测试加固,
性能基线见 [benchmarks.md](benchmarks.md))。

---

## 3. 快速上手

最小可运行示例(完整代码见 [examples/hello_tree](../examples/hello_tree/main.cpp)):

```cpp
#include <nebulabt/core/engine.hpp>
#include <nebulabt/sdk/action.hpp>
using namespace nebulabt;

// ① 一个同步业务节点
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

int main() {
  Engine engine;                                       // ② 引擎
  engine.registerAction("Greet", std::make_shared<Greet>());  // ③ 注册

  const char* xml = R"(                                 // ④ 树
  <root BTCPP_format="v4">
    <BehaviorTree ID="hello">
      <Greet who="{user}"/>
    </BehaviorTree>
  </root>)";

  TaskSpec spec;
  spec.slots["user"] = "nebula";                        // ⑤ slots 注入黑板
  auto task = engine.submitFromText(xml, spec);         // ⑥ 提交
  task.wait();                                          // ⑦ 等终态
}
```

七个步骤就是全部骨架:**定义节点 → 注册 → 写树 → 提交 → 等结果**。

---

## 4. 编写业务节点

继承 `NebulaAction`,实现三个部分。**只写业务逻辑,不感知 tick、线程、轮询。**

### 4.1 端口声明(providedPorts)

端口是节点的类型化输入输出,树 XML 通过 `{key}` 把端口映射到黑板:

```cpp
BT::PortsList providedPorts() const override {
  return {
    BT::InputPort<std::string>("target_item"),   // 必填输入
    BT::InputPort<int>("retry", 3, "重试次数"),   // 带默认值 + 说明
    BT::OutputPort<double>("price"),             // 输出
    BT::InputPort<std::vector<std::string>>("items"),  // 集合输入
  };
}
```

### 4.2 同步动作:直接返回终态

```cpp
BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
  auto item = ctx.in<std::string>("target_item");
  if (!lookup(item)) {
    ctx.fail("UNKNOWN_ITEM", "no prompt mapping for " + item);  // 结构化失败
    return BT::NodeStatus::FAILURE;
  }
  ctx.out("price", 3.14);        // 类型化写输出端口
  return BT::NodeStatus::SUCCESS;
}
```

### 4.3 异步动作:发起调用,回调续传

**关键约定:`start` 返回 RUNNING 表示"已发起",完成时在任意线程调 `done`。**

```cpp
BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
  const std::string point = ctx.in<std::string>("point_id");

  rpcClient->asyncNavigate(point, [done](ErrorCode ec, Result r) {
    if (ec) {
      done(BT::NodeStatus::FAILURE, {});          // 任意线程安全
    } else {
      done(BT::NodeStatus::SUCCESS, {{"duration", r.seconds}});
    }
  });
  return BT::NodeStatus::RUNNING;                 // 引擎挂起此节点,等 done 唤醒
}
```

- `done` 的 `result` JSON 中的键会**按端口声明的类型**写回 Output 端口
- 重复调用 `done` 只认第一次
- `start` 内抛异常会被捕获并记录 `ACTION_EXCEPTION` 失败

### 4.4 取消:halt()

任务取消或树被 halt 时,框架回调 `halt(ctx)`。**异步动作必须实现**:取消在途调用,否则资源泄漏/误回调:

```cpp
void halt(NebulaCtx& ctx) override {
  rpcClient->cancelNavigation();   // 取消在途请求;之后的 done 回调会被框架丢弃
}
```

### 4.5 NebulaCtx 全部能力

| 方法 | 说明 |
|---|---|
| `in<T>(port, fallback)` | 读输入端口;缺失/类型不符返回 fallback |
| `out<T>(port, value)` | 写输出端口(须在 ports 中声明) |
| `fail(code, message)` | 记录结构化失败(自动附带节点名/时间戳) |
| `inputsJson()` | 全部输入端口快照(调试/录制用) |
| `taskId()` / `taskType()` | 当前任务标识 |

---

## 5. 编写行为树

### 5.1 骨架

```xml
<root BTCPP_format="v4" main_tree_to_execute="main">
  <BehaviorTree ID="main">
    <Sequence name="root_flow">
      <!-- 步骤按顺序执行;任一失败则整体失败 -->
    </Sequence>
  </BehaviorTree>
</root>
```

- `<root>` 内可放多个 `<BehaviorTree>`,用 `main_tree_to_execute` 指定入口;其余可作为 `<SubTree ID="..."/>` 复用
- 节点标签 = `registerAction` 的注册名,或内置节点名

### 5.2 核心控制节点(BT.CPP 内置)

```xml
<Sequence>            <!-- 顺序:全成功才成功;子节点 RUNNING 则整体 RUNNING -->
<Fallback>            <!-- 选择:第一个成功的分支;常用于"成功路径 + 失败兜底" -->
<ReactiveSequence>    <!-- 每轮重估前置条件(条件变了会重新检查) -->
<Parallel success_count="2" failure_count="1"/>  <!-- 并行 -->
<Inverter><X/></Inverter>
<RetryUntilSuccessful num_attempts="5"><X/></RetryUntilSuccessful>
<Timeout msec="5000"><X/></Timeout>              <!-- 子节点超时则失败 -->
<Delay delay_msec="2000"><X/></Delay>            <!-- 延时启动 -->
<Script code="count := count + 1"/>              <!-- 黑板脚本 -->
<ScriptCondition code="count > 3"/>              <!-- 脚本条件 -->
<SubTree ID="xxx" param="{value}"/>              <!-- 子树复用 -->
```

### 5.3 NebulaBT 增强节点

**ForEach —— 集合遍历(数据驱动,消灭复制粘贴)**

```xml
<ForEach array="{locations}" element="{loc}" continue_on_failure="true">
  <Sequence>
    <NavigateTo point_id="{loc}"/>
    <DetectAt loc="{loc}" count="{count}"/>
    <RequireFound count="{count}"/>
  </Sequence>
</ForEach>
```
- `array`:黑板 `vector<string>`(或 `;` 分隔字符串);每轮把当前元素写入 `element`
- 子节点 FAILURE:默认整体失败;`continue_on_failure="true"` 时跳过该元素继续

**WhileLoop —— 真 while 循环(condition + body,非 reactive)**

```xml
<WhileLoop max_iterations="1000">
  <CheckHasWork/>     <!-- condition:SUCCESS 继续 / FAILURE 正常退出(整体 SUCCESS) -->
  <DoOneUnit/>        <!-- body:SUCCESS 后重估 condition;FAILURE 整体失败 -->
</WhileLoop>
```

**SwitchN —— 可变 case 数分发(比内置 Switch2~6 无上限)**

```xml
<SwitchN variable="{mode}" case_1="fast" case_2="safe">
  <FastPath/>          <!-- case_1 -->
  <SafePath/>          <!-- case_2 -->
  <DefaultPath/>       <!-- 全部未命中时执行 -->
</SwitchN>
```

**RetryWithDelay —— 带延时与墙钟预算的重试**

```xml
<RetryWithDelay attempts="5" delay_ms="3000" budget_ms="120000">
  <GraspStep/>
</RetryWithDelay>
```
- `attempts`:总执行次数(含首次);`delay_ms`:尝试间隔(到期由调度器唤醒,无轮询)
- `budget_ms`:墙钟预算,次数与时间**任一用尽**即失败——防"30 次重试跑半小时"

**Checkpoint —— 断点恢复标记**(见 [§9](#9-断点恢复))

```xml
<Checkpoint id="grasp_done">
  <GraspStep/>   <!-- 该子树 SUCCESS 后,全量黑板快照入 journal -->
</Checkpoint>
```

---

## 6. 提交任务

### 6.1 TaskSpec 全字段

```cpp
TaskSpec spec;
spec.task_id   = "order-20261105-001";  // 可选;缺省自动生成
spec.task_type = "DELIVER_ORDER";       // 注入黑板 task_type;pack 路由键
spec.slots     = json::object();        // 业务参数(见注入规则)
spec.schema    = json::object();        // 加载期校验(见 §7);空 = 不校验
spec.timeout_ms = 60000;                // 任务级超时;超时 halt + TASK_TIMEOUT
spec.resume_journal = json::array();    // 断点恢复(见 §9)
spec.pack_version = "1.2.0";            // 显式钉住树包版本(可选)
spec.watchdog_ms = 5000;                // 卡死检测(见 6.5);0 = 跟随引擎默认
spec.resources = {"gripper", "bin_3"};  // 互斥资源声明(见 6.6);冲突提交被拒
spec.groot2_port = 1667;                // Groot2 实时可视化(0 = 关,默认)
```

### 6.2 slots 注入规则

| slots 中的 JSON 类型 | 黑板中的类型 |
|---|---|
| string | string |
| array | `vector<string>`(ForEach 直接可用) |
| int/double/bool | **schema 声明了对应类型时**按真实类型注入;否则 dump 为 string |

### 6.3 三种提交方式

```cpp
engine.submitFromText(xml, spec);            // XML 字符串(适合小树/示例)
engine.submitFromFile(path, spec);           // XML 文件
engine.submitTask(spec);                     // 树包路由(推荐,见 §10)
```

### 6.4 任务句柄

```cpp
auto task = engine.submitFromText(xml, spec);

task.wait();                       // 阻塞等终态(测试/示例)
task.waitFor(5s);                  // 带超时;超时返回当前状态(通常仍 RUNNING)
task.state();                      // QUEUED/RUNNING/SUCCEEDED/FAILED/CANCELED
task.cancel();                     // 取消:halt 全树 → 各动作 halt() → CANCELED
task.failures();                   // 结构化失败快照
task.journal();                    // 事件日志快照(可整体作为 resume_journal)
task.explain();                    // 完整执行报告(见 §8)

engine.onFinished([](TaskHandle t) { /* 终态回调,调度线程执行,应快速返回 */ });
```

### 6.5 watchdog:卡死检测(M4)

崩溃可恢复,卡死必须可发现。任务 RUNNING 期间**无任何节点状态迁移**超过阈值
→ 告警回调;可选 halt + `TASK_WATCHDOG` 终态:

```cpp
engine.setWatchdog(std::chrono::milliseconds(5000),
                   /*halt_on_trigger=*/true,            // false = 只告警
                   [](const std::string& task_id, uint64_t quiet_ms) {
                     alertOps("task " + task_id + " stuck for " +
                              std::to_string(quiet_ms) + " ms");
                   });
// 单任务覆盖:spec.watchdog_ms(引擎默认之上的例外)
```

同一静默期只触发一次;活动恢复自动重新武装。典型用法:机器人任务卡在
失联的硬件调用上,watchdog 先于客户投诉发现。

### 6.6 资源上限与互斥(M4/M5)

长期运行服务的内存必须可预算:

```cpp
EngineLimits limits;
limits.max_active_tasks   = 64;    // 活动任务上限(超限提交拒绝)
limits.max_pending_jobs   = 1024;  // 调度队列上限(失控回调风暴被拒)
limits.max_pending_timers = 256;   // 定时器上限
limits.max_journal_events = 1000;  // 单任务内存 journal 上限(0=不限;
                                   // 触顶告警,sink 落盘不受影响)
engine.setLimits(limits);
```

任务级互斥:声明 `spec.resources = {"gripper"}`,与任一活动任务声明集
有交集 → 提交被拒绝(返回空句柄);任务终态自动释放。防"两个任务同时
抓同一个包裹"。

---

## 7. Schema:让错误在加载期爆

提交时可选携带 schema,**两层校验全部发生在任务启动前**:

```cpp
spec.schema = {
  { "slots", {
      { "target_item",     { { "type", "string" }, { "required", true } } },
      { "target_location", { { "type", "string" }, { "required", false } } },
      { "grasp_retry",     { { "type", "int" },    { "required", false } } },
  }}
};
```

- **L1 提交 ↔ schema**:required 缺失、**未知字段**(拼写错误!)、类型不符 → 提交被拒绝;声明的 int/double/bool 按真实类型注入黑板
- **L2 树 ↔ schema(数据流检查)**:树里每个被读的黑板键,必须是 schema 声明的键、框架注入键(`task_id`/`task_type`)、或树内某节点 Output 写出的键——键名拼错、注入了没人读,加载期报错并精确到节点和端口:

```
error: node 'ForEach' port 'array' reads undeclared blackboard key 'locaitons'
```

不提供 schema = 校验关闭,完全向后兼容。**强烈建议始终提供**。

---

## 8. 观测与排障

### 8.1 explain():一条命令还原"为什么"

```json
{
  "task_id": "task-1793-0", "task_type": "DELIVER_ORDER",
  "pack": "pick_and_deliver", "pack_version": "1.0.0",
  "state": "FAILED",
  "tree": {
    "name": "deliver_main", "type": "Sequence", "status": "IDLE",
    "children": [
      { "name": "GraspItem", "type": "GraspItem", "status": "IDLE",
        "attempts": 3, "running_ms": 96.4, "first_ms": 120.1, "last_ms": 500.2,
        "children": [] }
    ]
  },
  "failures": [
    { "code": "GRASP_FAILED", "node": "GraspItem", "node_type": "GraspItem",
      "message": "gripper timeout, attempt 2", "timestamp": "..." }
  ]
}
```

> M7 起任务 **RUNNING 期间调用 explain() 也安全**:树遍历经调度线程
> 快照完成(2s 上限,调度线程自调用或队列满时降级为
> `tree_snapshot: unavailable`)——排障恰好发生在任务卡住时最需要它。

### 8.2 结构化失败

- 节点里 `ctx.fail(code, msg)`;忘了调?框架自动兜底记 `NODE_FAILURE`
- 失败自动富化(M7):每条记录附带激活时刻的**输入端口快照**,排障不用猜"当时参数是什么"
- 异步回调线程无法安全访问 ctx?done 结果携带保留键
  `{"__fail_code": "...", "__fail_message": "..."}`,桥在调度线程代记——
  见 LlmDecision 的用法
- 建议错误码全大写下划线:`NAV_TIMEOUT` / `PAY_DECLINED`;同一框架内保持词表一致

### 8.2.1 失败码登记表(M8)

失败码是接口,先声明后使用:

```cpp
engine.declareFailureCodes({
  {"NAV_TIMEOUT",     "navigation exceeded time budget"},
  {"PAY_DECLINED",    "payment provider declined the charge"},
});
// pack 清单也可声明:task_types.XXX.failure_codes = { "NAV_TIMEOUT": "..." }
```

登记表启用后,`ctx.fail` 用了未登记的码 → 该条 FailureRecord 标记
`undeclared_code: true`(explain 可筛出拼写错误)。表为空 = 不校验。

### 8.3 journal

事件类型:`submitted → started → checkpoint → finished`(恢复时含
`checkpoint_restored`,失败含 `compensation_*` / `guard_violated`)。

**落盘推荐走帧格式**(M4,掉电安全):每事件一帧
`[u32 len][u32 crc32][payload]`,崩溃瞬间最多损坏最后一帧,读取容错
截断到完好前缀并标记 `torn_tail`:

```cpp
#include <nebulabt/core/journal_file.hpp>

nebulabt::JournalFileWriter w;
w.open("run.bin", /*append=*/false,
       nebulabt::JournalFsync::PerEvent);       // Never = 只 fflush(flash 友好)
engine.setJournalSink([&w](const JournalEvent& e) { w.append(e); });

// 恢复侧:读回完好前缀,直接作为 resume_journal
nlohmann::json events;
nebulabt::JournalFileReadStats st;
nebulabt::readJournalFile("run.bin", events, &st);   // st.torn_tail = 掉电窗口
```

裸 JSONL sink(逐行 `dump()`)仍可用,但掉电窗口内的半行只能靠解析容错。

### 8.4 CLI

```bash
nebula validate <tree.xml> [schema.json] [--json]  # 加载期校验 → CI 门禁;
                                                   # --json 机器可读(LLM 反馈闭环)
nebula journal  <file>            # 检查落盘 journal(帧/JSONL 自适应;
                                  # 坏行统计/事件分布/检查点清单)
nebula replay   <file> [--until N] # 恢复态预演:哪些检查点会被跳过、
                                   # 恢复哪些黑板键、上轮终态;--until 时间旅行
nebula explain  <file> [task_id]  # 事后分析:时间线+终态+崩溃识别
nebula doc      <tree.xml>        # 树 → mermaid 流程图(评审/交接)
```

### 8.5 Groot2 实时可视化(M7)

```cpp
spec.groot2_port = 1667;   // 提交时开启;端口被占用只告警不影响任务
```
用 [Groot2](https://www.behaviortree.cc/) 连接即可实时观看树执行
(需要 BT.CPP 编译时开启 GROOT_INTERFACE)。

---

## 9. 断点恢复

### 9.1 用法

```xml
<Sequence>
  <SearchStep/>                        <!-- 有副作用的段 -->
  <Checkpoint id="search_done"/>
  <GraspStep/>
  <Checkpoint id="grasp_done"/>
  <HandoverStep/>                      <!-- 可能失败的后续段 -->
</Sequence>
```

规则:**Checkpoint 包住"一段已完成的副作用"**;它是装饰器,恰好一个子节点。

```cpp
// 第一轮:失败在 Checkpoint 之后 → FAILED;journal 已含检查点
auto t1 = engine.submitTask(spec);
t1.wait();

// 第二轮:带 journal 恢复(注意:恢复自动钉住原 pack 版本)
spec.resume_journal = t1.journal();          // 或从落盘文件读回
auto t2 = engine.submitTask(spec);
// search_done/grasp_done 命中 → 子树整体跳过、黑板快照恢复 → 只重跑失败段
```

### 9.2 语义与前提

- 恢复时命中的检查点:**子节点不执行**(上轮副作用不重复),黑板恢复为快照值
- **前提:节点幂等或其副作用已随检查点提交**。原则:`query` 类可重放;`charge`/`grasp` 类必须放在检查点保护内
- 多跳恢复成立:恢复轮的 journal 会延续完成历史,可继续传给第三轮
- 跨进程恢复:sink 落盘 → 新进程读回 → `resume_journal`;崩溃瞬间写坏的最后一行解析失败会被容错跳过
- 恢复轮建议同时保留原 `task_id`(便于审计串联)

---

## 10. 树包与灰度

### 10.1 pack.json 完整参考

```
mypack/
├── pack.json          # 规范格式(零依赖)
└── trees/*.xml
```

**YAML 清单(可选)**:`-DNEBULABT_WITH_YAML=ON`(需 yaml-cpp)后,
`pack.yaml` 与 pack.json 完全等价、字段一致——注释即文档,ROS 生态友好:

```yaml
# mypack/pack.yaml —— 注释随配置走,新增任务加一条即可
name: delivery
version: "1.0.0"
task_types:
  DELIVER_ORDER:
    tree: deliver_order
    schema:
      slots:
        target: { type: string, required: true }
    failure_codes:
      LOCKER_BUSY: 目标柜格被占用
```

引用规范(pack.json 优先;二者并存时 JSON 胜出)。

```json
{
  "name": "delivery",
  "version": "1.2.0",
  "trees_dir": "trees",
  "requires": [ {"pack": "common", "min_version": "1.0.0"} ],
  "templates": {
    "deliver": { "tree": "deliver_item", "schema": {...}, "timeout_ms": 60000 }
  },
  "task_types": {
    "DELIVER_ORDER":      { "template": "deliver" },
    "DELIVER_ORDER_FAST": { "template": "deliver", "slot_defaults": {"grasp_retry": 1} }
  }
}
```

- **模板 + slot_defaults**:变体是模板的实例(改默认参数),不是 fork 一棵新树
- **requires**:依赖 pack 未加载或版本不达标时 loadPack 抛异常

### 10.2 加载与路由

```cpp
engine.registerAction("NavigateTo", ...);   // 先注册节点
engine.loadPack("packs/delivery/1.2.0");    // 加载即校验全部树(失败抛异常)
// CI 中加载所有 pack = 最强回归门禁

TaskSpec spec; spec.task_type = "DELIVER_ORDER";
engine.submitTask(spec);                    // schema/timeout/slot_defaults 自动合并

engine.setCanary("delivery", "1.3.0", 10);  // 10% 新任务走灰度版本
```

- 路由优先级:**pack_version 显式钉住 > 灰度命中 > 最高版本**
- 恢复重跑自动钉住原版本(防灰度翻转导致检查点失配)
- 同名同版本重复加载抛异常

---

## 11. 树级测试

用 `testkit::ScriptedAction` 替换真实适配器,让整棵树在 CI 里跑:

```cpp
#include <nebulabt/testkit/mock.hpp>

auto nav = std::make_shared<testkit::ScriptedAction>(
    BT::PortsList{ BT::InputPort<std::string>("point_id") });
nav->thenFail("NAV_TIMEOUT", "no route")                       // 第 1 次:失败
    .then(BT::NodeStatus::SUCCESS, {{"arrived", true}});       // 第 2 次:成功
engine.registerAction("Nav", nav);

// ... 提交树,断言:
CHECK(task.wait() == TaskState::Succeeded);      // 重试挽回
CHECK(nav->calls().size() == 2);                 // 输入快照被录制
CHECK(nav->calls()[0]["point_id"] == "kitchen");
```

建议的最小测试矩阵:金路径 / 每个失败分支的兜底 / 重试挽回 / 取消 / 超时 / 恢复重跑副作用不重复。

### 11.1 故障注入(M6):不改生产代码

```cpp
#include <nebulabt/testkit/inject.hpp>

engine.registerAction("ChargePayment",
    nebulabt::testkit::flaky(realPaymentAction, /*fail_first=*/2));
// 前 2 次激活失败(INJECTED_FAILURE),之后透传真实动作;
// 计数跨 Engine 存活——同一实例可用于"崩溃 → 恢复多轮"的故障贯穿
```

### 11.2 虚拟时钟(M6):时间敏感场景零等待

```cpp
auto clock = std::make_shared<nebulabt::VirtualClock>();
nebulabt::Engine engine(clock);        // 虚拟时钟模式
auto task = engine.submitFromText(retryXml, spec);   // RetryWithDelay delay_ms=1000
engine.advanceTime(1000);              // 重试即刻到期,测试不真睡
```

超时/watchdog/重试延时/活动戳全部走注入时钟;journal 时间戳仍为真实墙钟。
配套 `tests/test_property_recovery`:随机虚拟时间推进 × 崩溃 × 恢复 ×
200 轮,断言检查点副作用恰好一次——恢复逻辑的混沌测试。

---

## 12. 内置节点参考

### NebulaBT 提供

| 节点 | 端口 | 语义 |
|---|---|---|
| `ForEach` | array, element(out), continue_on_failure | 集合遍历;元素写入黑板;失败默认上抛 |
| `WhileLoop` | max_iterations(默认 1000) | condition+body 真 while;body SUCCESS 后重估条件;condition FAILURE = 正常退出(SUCCESS) |
| `SwitchN` | variable, case_1..case_32 | 分发;default 恒为最后一个子节点 |
| `RetryWithDelay` | attempts, delay_ms, budget_ms | 重试;次数与墙钟任一用尽即失败 |
| `Checkpoint` | id | 子节点成功后拍全量黑板快照;恢复时跳过并还原 |
| `Compensable` | id | **双子:[主工作, 补偿]**。主工作 SUCCESS 即武装;任务 FAILED 时引擎**逆序自动执行**已武装补偿(超时路径同,取消不触发);补偿失败记 `COMPENSATION_FAILED` 并继续逆序 |
| `ReactiveGuard` | while_true, period_ms(默认 100) | 运行期守护:进入前与子树 RUNNING 期间持续检查黑板 bool 键;false/缺失(fail-closed)→ halt 子树 + `GUARD_VIOLATED` + FAILURE。安全区/e-stop 刚需 |
| `LLMDecision` | prompt, branches, branch(out), reason(out) | LLM 只做"选分支":客户端经能力接口注入(核心零 HTTP 依赖);输出不在 branches 集合内 → `LLM_INVALID_BRANCH`(幻觉拦在节点边界)。见 §16 |

`Compensable` 用法(Saga:扣了 A、B 失败,忘补偿 A 在结构上不可能发生):

```xml
<Sequence>
  <Compensable id="reserve">
    <ReserveStock/>          <!-- 主工作(有副作用) -->
    <ReleaseStock/>          <!-- 补偿:正常运行永不执行;声明即生效 -->
  </Compensable>
  <Ship/>                    <!-- 这里失败 → 引擎自动逆序补偿 -->
</Sequence>
```

### BT.CPP 4.9 内置(常用子集)

| 类别 | 节点 |
|---|---|
| 控制 | Sequence, Fallback, Parallel, ReactiveSequence, ReactiveFallback, IfThenElse, WhileDoElse, Switch2~6, SubTree |
| 装饰 | Inverter, Timeout(msec), Delay(delay_msec), RetryUntilSuccessful(num_attempts), Repeat, RunOnce, ForceSuccess, ForceFailure, Precondition |
| 动作/条件 | Script(code), ScriptCondition(code), SetBlackboard, Sleep(msec), AlwaysSuccess, AlwaysFailure, WasEntryUpdated |

---

## 13. 线程模型

- **调度线程**:唯一的树 tick 执行线程;所有节点 `start`/`halt` 都在其中
- **`done` 回调**:可在**任意线程**调用;框架自动投递唤醒,结果类型化写回端口;
  跨线程失败走 `__fail_code` 保留键,不要在回调线程里触碰 ctx
- **不需要锁**:节点内部状态只在调度线程被触碰(除非你在自己的异步线程里读写共享业务对象——那是你的锁)
- **线程安全承诺**:TaskHandle 的 state/failures/journal/explain、
  Engine 的注册与提交(内部已串行化树创建;多引擎实例之间天然并行)
- **explain()**:RUNNING 期间安全(M7 调度线程快照);唯调度线程自调用
  会降级为无树快照
- **Engine 析构**:自动取消全部活动任务并排空调度器,`wait()` 不会挂死

---

## 14. 错误码约定

| 错误码 | 来源 | 含义 |
|---|---|---|
| `NODE_FAILURE` | 框架 | 动作返回 FAILURE 但未调 `ctx.fail()`(兜底) |
| `ACTION_EXCEPTION` | 框架 | `start` 内抛出未捕获异常 |
| `TICK_EXCEPTION` | 框架 | 树 tick 抛出异常(如 Compensable 子树数不符) |
| `TASK_TIMEOUT` | 框架 | 任务级超时触发 |
| `TASK_WATCHDOG` | 框架 | watchdog 静默超限且 halt_on_trigger 开启 |
| `GUARD_VIOLATED` | 框架 | ReactiveGuard 条件破坏(fail-closed) |
| `COMPENSATION_FAILED` | 框架 | 补偿子树未成功;不影响其余补偿继续 |
| `LLM_INVALID_BRANCH` | 框架 | LLM 输出不在允许分支集(幻觉拦截) |
| `LLM_NO_BRANCHES` | 框架 | LLMDecision 的 branches 端口为空 |
| `PORT_OUTPUT_ERROR` | SDK | `ctx.out` 写端口失败(端口未声明等) |
| 你的业务码 | 你 | 全大写下划线;**建议进登记表**(见 8.2.1) |

终态 reason 参照:`tree returned FAILURE` / `task timeout` /
`task watchdog` / `canceled by request`。

---

## 15. 最佳实践与反模式

**做:**

1. **树里只有业务编排**——重试/超时/降级交给装饰器与策略,不写业务 if-else
2. **失败必须可解释**——`ctx.fail` 带上业务上下文("no water bottle at shelf_b" 优于 "failed")
3. **每个 Checkpoint 对应一个真实业务安全点**——副作用已落库/已物理完成的位置
4. **pack + schema 常态化**——提交校验 + CI validate 是你的回归防线
5. **异步动作必实现 halt**——取消路径的资源清理由此驱动
6. **循环体记得 max_iterations 兜底**(WhileLoop 有默认 1000,够用但要知道)

**不做:**

1. **不在 `start` 里阻塞**(sleep/同步 RPC)——返回 RUNNING + done,让引擎保持响应
2. **不轮询**——需要等待就 `done` 或 `RetryWithDelay`;黑轮询会让事件驱动失效
3. **不把一棵树写成 300 行**——层级化:子树 + 端口参数化,单屏能读完一棵树
4. **不用黑板当全局变量转储**——键要进 schema,数据流要能从树上读出来
5. **不让非幂等节点裸露在 Checkpoint 之外**再谈恢复
6. **不手写补偿分支**——用 `Compensable` 声明,让引擎逆序执行
7. **不在回调线程里触碰 ctx**——跨线程失败走 `__fail_code` 保留键

---

## 16. Agent 骨架:LLM 提议,守护裁决(M8)

LLM 决策纳入结构化控制流——**LLM 只做"选分支",控制流仍在树里**:

```cpp
#include <nebulabt/nodes/llm_decision.hpp>

// 能力接口注入你的 LLM 客户端(核心库零 HTTP 依赖)
LlmDecision::Client client = [](const nlohmann::json& req, Done done) {
  myLlm.complete(req.dump(), [done](std::string text) {   // 任意线程
    done(BT::NodeStatus::SUCCESS, nlohmann::json::parse(text));
  });
};
engine.registerAction("LLMDecision", std::make_shared<LlmDecision>(client));
```

```xml
<Sequence>
  <SenseWorld situation="{situation}" in_combat="{in_combat}"/>
  <LLMDecision prompt="{situation}" branches="patrol,engage,retreat"
               branch="{decision}" reason="{why}"/>
  <SwitchN variable="{decision}" case_1="patrol" case_2="engage" case_3="retreat">
    <WhileLoop>...</WhileLoop>
    <ReactiveSequence>                                  <!-- 守护段 -->
      <SenseWorld in_combat="{in_combat}"/>             <!-- 每 tick 刷新感知 -->
      <ReactiveGuard while_true="{in_combat}" period_ms="30">
        <Engage/>                                       <!-- 条件破坏即 halt -->
      </ReactiveGuard>
    </ReactiveSequence>
    <Retreat/>
  </SwitchN>
</Sequence>
```

这个组合的性质(完整可跑示例见 `examples/game_npc` 场景 E):

- **幻觉拦截**:输出不在 branches 集合 → `LLM_INVALID_BRANCH`,污染不了控制流
- **守护裁决**:决策不可信的前提是守护条件持续成立——玩家撤离时
  ReactiveGuard 否决 LLM 的 engage 决策、halt 在途工作(`GUARD_VIOLATED`)
- **可恢复/可审计**:Checkpoint 让长 agent 任务崩溃后续跑;explain 输出
  决策 + 违规的完整证据链(LLM 决策不可复现,轨迹是唯一审计依据)
- **生成护栏**:`nebula validate --json` 输出机器可读错误——
  LLM 生成树草稿 → 校验 → 错误回灌修复 → 人审入库

---

## 17. 0 编译新增任务:通用宿主(参考 examples/zero_compile)

任务类型 = **通用原语动作的组合 + pack 资产**。宿主只编译一次,新增任务
不改代码、不重编译:

```bash
./build/examples/zero_compile/task_host examples/zero_compile/packs
# REPL:
#   list                        列出已加载的任务类型
#   {"task_type": "PATROL_AREA", "slots": {...}}   提交任务
#   load <dir>                  运行时加载新树包(丢进 packs/ 即生效)
```

宿主内置的通用原语:`Log`(支持 `{key}` 文案插值)/ `Wait` / `SetSlot` /
`CheckSlot`(eq/ne/gt/lt/contains)/ `Fail` + 全部内置节点(Checkpoint/
Compensable/ReactiveGuard/RetryWithDelay/ForEach/WhileLoop/SwitchN)。

配套 SDK 能力:`ctx.inRef(port)` 取端口绑定的键名、`ctx.peek(key)` 按名
读黑板——支撑"按键名检查任意槽位"的条件原语。

要点(来自 [examples/zero_compile](../examples/zero_compile)):
- 新任务 = `packs/` 下一个目录(pack.json 或 pack.yaml)+ 一棵树,
  `load` 即时生效,加载期校验照常把关
- 通用原语是你的**能力面**:需要新能力(如 HTTP 调用、话题发布)时
  才写一次 C++,之后所有任务复用
- demo 的 CTest(`zero_compile`)在 CI 常驻:运行中 `load` 新包 →
  新任务类型端到端跑通(含失败路径的重试 + 自动补偿)

---

## 附:常见问题

**Q: 任务卡在 RUNNING 不动?**
`nebula explain <journal>` —— 无 finished 事件即中途崩溃;检查是否有节点既不返回终态也不调 done。

**Q: 恢复后某段还是重跑了?**
确认该段是否在 Checkpoint 之内、检查点 id 是否与上轮一致、resume 时 pack_version 是否被钉住(框架自动,但显式覆盖会失效)。

**Q: 端口值是空的?**
开 L2 校验:键名拼写、是否被某个 Output 写出、slots 是否真的传了——加载期全都会告诉你。

---

*本文档对应 v1.0.0;API 变更会同步更新并记录于 CHANGELOG.md。*
