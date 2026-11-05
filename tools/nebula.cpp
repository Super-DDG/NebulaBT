// nebula CLI — NebulaBT 命令行工具
//
// 用法:
//   nebula validate <tree.xml> [schema.json]
//     加载期校验:树可解析、节点已注册、端口引用的黑板键可溯源
//     (schema 声明 / 框架注入 / 树内 Output 写出)。供 CI 门禁使用。
//   nebula journal <file.jsonl>
//     检查持久化的 journal 文件(每行一个事件 JSON):解析容错统计、
//     事件类型分布、检查点清单——恢复前排查用。
//
// 规划中的子命令:explain / replay / doc(见 docs/DESIGN.md 路线图)

#include <nebulabt/core/engine.hpp>
#include <nebulabt/core/journal.hpp>

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

static bool readFile(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

static int cmdValidate(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: nebula validate <tree.xml> [schema.json]\n";
    return 2;
  }
  std::string xml;
  if (!readFile(argv[2], &xml)) {
    std::cerr << "error: cannot read " << argv[2] << "\n";
    return 2;
  }
  nlohmann::json schema = nlohmann::json::object();
  if (argc >= 4) {
    std::string schemaText;
    if (!readFile(argv[3], &schemaText)) {
      std::cerr << "error: cannot read " << argv[3] << "\n";
      return 2;
    }
    try {
      schema = nlohmann::json::parse(schemaText);
    } catch (const std::exception& e) {
      std::cerr << "error: schema parse: " << e.what() << "\n";
      return 2;
    }
  }

  nebulabt::Engine engine;
  const auto errors = engine.validate(xml, schema);
  if (errors.empty()) {
    std::cout << argv[2] << ": OK\n";
    return 0;
  }
  for (const auto& e : errors) {
    std::cerr << "error: " << e << "\n";
  }
  std::cerr << argv[2] << ": " << errors.size() << " error(s)\n";
  return 1;
}

// nebula journal <file.jsonl>:检查持久化 journal(恢复前排查)
static int cmdJournal(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: nebula journal <file.jsonl>\n";
    return 2;
  }
  std::ifstream f(argv[2]);
  if (!f) {
    std::cerr << "error: cannot read " << argv[2] << "\n";
    return 2;
  }

  int ok = 0, bad = 0, tasks = 0;
  std::string lastTask;
  std::map<std::string, int> byEvent;
  std::map<std::string, std::string> lastState;  // task_id -> finished state
  std::map<std::string, std::string> lastType;
  std::vector<std::string> checkpoints;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    try {
      const auto j = nlohmann::json::parse(line);
      nebulabt::JournalEvent e = j.get<nebulabt::JournalEvent>();
      if (e.task_id.empty() || e.event.empty()) {
        throw std::runtime_error("missing task_id/event");
      }
      ++ok;
      ++byEvent[e.event];
      if (e.task_id != lastTask) {
        ++tasks;
        lastTask = e.task_id;
      }
      lastType[e.task_id] = e.task_type;
      if (e.event == "checkpoint" && e.data.contains("id")) {
        checkpoints.push_back(e.data["id"].get<std::string>());
      }
      if (e.event == "finished" && e.data.contains("state")) {
        lastState[e.task_id] = e.data["state"].get<std::string>();
      }
    } catch (const std::exception& err) {
      ++bad;
      std::cerr << "warning: bad line ("
                << err.what() << "): "
                << line.substr(0, 60) << (line.size() > 60 ? "..." : "")
                << "\n";
    }
  }

  std::cout << "file: " << argv[2] << "\n"
            << "events ok: " << ok << ", bad lines: " << bad << "\n"
            << "tasks: " << tasks << "\n";
  for (const auto& [ev, n] : byEvent) {
    std::cout << "  " << ev << ": " << n << "\n";
  }
  for (const auto& [id, st] : lastState) {
    std::cout << "task " << id << " (" << lastType[id] << "): " << st << "\n";
  }
  if (!checkpoints.empty()) {
    std::cout << "checkpoints recorded:";
    for (const auto& c : checkpoints) std::cout << " " << c;
    std::cout << "\n";
  }
  return bad == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: nebula <command> [args]\n"
              << "commands:\n"
              << "  validate <tree.xml> [schema.json]\n"
              << "  journal  <file.jsonl>\n";
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "validate") {
    return cmdValidate(argc, argv);
  }
  if (cmd == "journal") {
    return cmdJournal(argc, argv);
  }
  std::cerr << "error: unknown command '" << cmd << "'\n";
  return 2;
}
