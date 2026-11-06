// ============================================================================
// zero_compile:0 编译新增任务演示
//
// 宿主(task_host)只编译一次,内置一组**通用原语动作**:
//   Log / Wait / SetSlot / CheckSlot / Fail
// 加上框架内置节点(ForEach/WhileLoop/SwitchN/RetryWithDelay/Checkpoint/
// Compensable/ReactiveGuard),组合能力由 pack 资产提供。
//
// 新增任务类型 = 往 packs/ 丢一个目录(pack.json 或 pack.yaml + trees/),
// REPL 里 `load <dir>` 即时生效——不改代码、不重编译。
//
// REPL 指令(每行一条):
//   list                          列出可用任务类型
//   {"task_type": "...", "slots": {...}}   提交任务并等待终态
//   load <dir>                    运行时加载新树包
//   quit                          退出
// ============================================================================

#include <nebulabt/core/engine.hpp>
#include <nebulabt/sdk/action.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace nebulabt;
using BT::NodeStatus;

// ---------------------------------------------------------------------------
// 通用原语动作(宿主的能力面;新任务不再需要新的 C++)
// ---------------------------------------------------------------------------

// 打日志(演示中即"对外可见的副作用")
// 支持 {key} 插值:文案中的黑板键替换为其值(BT.CPP 只解析整值引用,
// 文案内插由这里完成;键缺失保留原样)
class Log : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("message") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    const std::string raw = ctx.in<std::string>("message");
    std::string out;
    size_t pos = 0;
    while (true) {
      const size_t open = raw.find('{', pos);
      if (open == std::string::npos) {
        out += raw.substr(pos);
        break;
      }
      const size_t close = raw.find('}', open + 1);
      if (close == std::string::npos) {
        out += raw.substr(pos);
        break;
      }
      const std::string key = raw.substr(open + 1, close - open - 1);
      const nlohmann::json v = ctx.peek(key);
      if (v.is_null()) {
        out += raw.substr(pos, close - pos + 1);  // 键不存在:保留原样
      } else {
        out += raw.substr(pos, open - pos);
        out += v.is_string() ? v.get<std::string>() : v.dump();
      }
      pos = close + 1;
    }
    std::cout << "    [log] " << out << "\n";
    return NodeStatus::SUCCESS;
  }
};

// 真实等待(真实机器人中换成"导航到位"等长耗时动作)
class Wait : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<int>("delay_ms") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done& done) override {
    const int ms = ctx.in<int>("delay_ms");
    std::thread([ms, done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      done(NodeStatus::SUCCESS, {});
    }).detach();
    return NodeStatus::RUNNING;
  }
};

// 写黑板键(to 端口绑定目标键,如 to="{dest_floor}")
class SetSlot : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::OutputPort<std::string>("to"),
             BT::InputPort<std::string>("value") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    ctx.out("to", ctx.in<std::string>("value"));
    return NodeStatus::SUCCESS;
  }
};

// 条件检查:key 端口绑定要检查的黑板键(如 key="{parcel_ready}"),
// op ∈ eq/ne/gt/lt/contains;gt/lt 按数值比较,失败语义化
class CheckSlot : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    // key 声明为无类型端口:{键} 绑定不把黑板键钉成 string(与
    // ReactiveGuard 同法),否则 bool/int 槽位会与声明冲突
    return { BT::InputPort("key", "要检查的黑板键,如 {parcel_ready}"),
             BT::InputPort<std::string>("op"),
             BT::InputPort<std::string>("value") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    // key 端口绑定要检查的黑板键:key="{parcel_ready}"
    const std::string keyName = ctx.inRef("key");
    if (keyName.empty()) {
      ctx.fail("CHECK_BAD_KEY", "key port must reference a blackboard key");
      return NodeStatus::FAILURE;
    }
    const nlohmann::json v = ctx.peek(keyName);
    const std::string actual = v.is_null()      ? std::string()
                               : v.is_string()  ? v.get<std::string>()
                                                : v.dump();
    const std::string op = ctx.in<std::string>("op");
    const std::string expected = ctx.in<std::string>("value");
    bool ok = false;
    if (op == "eq") ok = actual == expected;
    else if (op == "ne") ok = actual != expected;
    else if (op == "contains") ok = actual.find(expected) != std::string::npos;
    else if (op == "gt" || op == "lt") {
      try {
        const double a = std::stod(actual), b = std::stod(expected);
        ok = op == "gt" ? a > b : a < b;
      } catch (const std::exception&) {
        ctx.fail("CHECK_NOT_NUMERIC",
                 "'" + keyName + "'=" + actual + " is not numeric");
        return NodeStatus::FAILURE;
      }
    } else {
      ctx.fail("CHECK_BAD_OP", "unknown op '" + op + "'");
      return NodeStatus::FAILURE;
    }
    if (!ok) {
      ctx.fail("CHECK_FAILED",
               keyName + "='" + actual + "' failed " + op + " '" + expected + "'");
    }
    return ok ? NodeStatus::SUCCESS : NodeStatus::FAILURE;
  }
};

// 模拟业务失败(演示重试/补偿路径)
class Fail : public NebulaAction {
public:
  BT::PortsList providedPorts() const override {
    return { BT::InputPort<std::string>("code"),
             BT::InputPort<std::string>("message") };
  }
  BT::NodeStatus start(NebulaCtx& ctx, const Done&) override {
    ctx.fail(ctx.in<std::string>("code"), ctx.in<std::string>("message"));
    return NodeStatus::FAILURE;
  }
};

// ---------------------------------------------------------------------------
// 宿主
// ---------------------------------------------------------------------------

static void submitAndReport(Engine& engine, const nlohmann::json& req) {
  TaskSpec spec;
  spec.task_type = req.value("task_type", "");
  if (auto it = req.find("slots"); it != req.end() && it->is_object()) {
    spec.slots = *it;
  }
  auto task = engine.submitTask(spec);
  if (!task) {
    std::cout << "[task] " << spec.task_type << " -> SUBMIT_REJECTED\n";
    return;
  }
  const TaskState st = task.wait();
  std::cout << "[task] " << spec.task_type << " -> " << toString(st) << "\n";
  for (const auto& f : task.failures()) {
    std::cout << "    [fail] " << f.code << ": " << f.message << "\n";
  }
  std::string events;
  for (const auto& e : task.journal()) {
    events += e.event + " ";
  }
  std::cout << "    [journal] " << events << "\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: task_host <packs_dir>\n";
    return 2;
  }
  const std::string packsDir = argv[1];

  Engine engine;
  engine.registerAction("Log", std::make_shared<Log>());
  engine.registerAction("Wait", std::make_shared<Wait>());
  engine.registerAction("SetSlot", std::make_shared<SetSlot>());
  engine.registerAction("CheckSlot", std::make_shared<CheckSlot>());
  engine.registerAction("Fail", std::make_shared<Fail>());
  engine.setLogger([](LogLevel, const std::string&) {});  // 静音引擎日志

  // 启动即扫描 packs/ 全部子目录加载(排序保证确定性)
  std::set<std::string> loadedDirs;
  if (std::filesystem::exists(packsDir)) {
    for (const auto& entry : std::filesystem::directory_iterator(packsDir)) {
      if (entry.is_directory()) {
        try {
          engine.loadPack(entry.path().string());
          loadedDirs.insert(entry.path().string());
        } catch (const std::exception& e) {
          std::cout << "[load] FAILED " << entry.path().string()
                    << ": " << e.what() << "\n";
        }
      }
    }
  }
  std::cout << "[host] ready, " << loadedDirs.size() << " pack(s)\n";
  for (const auto& t : engine.loadedTaskTypes()) {
    std::cout << "[type] " << t.task_type << "  (" << t.pack << " v"
              << t.version << ")\n";
  }

  // REPL
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line == "quit") break;
    if (line == "list") {
      for (const auto& t : engine.loadedTaskTypes()) {
        std::cout << "[type] " << t.task_type << "  (" << t.pack << " v"
                  << t.version << ")\n";
      }
      continue;
    }
    if (line.rfind("load ", 0) == 0) {
      const std::string dir = line.substr(5);
      try {
        engine.loadPack(dir);
        std::cout << "[load] ok " << dir << "\n";
        for (const auto& t : engine.loadedTaskTypes()) {
          std::cout << "[type] " << t.task_type << "  (" << t.pack << " v"
                    << t.version << ")\n";
        }
      } catch (const std::exception& e) {
        std::cout << "[load] FAILED " << dir << ": " << e.what() << "\n";
      }
      continue;
    }
    try {
      submitAndReport(engine, nlohmann::json::parse(line));
    } catch (const std::exception& e) {
      std::cout << "[input] not a command or JSON: " << e.what() << "\n";
    }
  }
  return 0;
}
