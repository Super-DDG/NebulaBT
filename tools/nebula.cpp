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

#include <tinyxml2.h>

#include <cstdio>
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

// ---------------------------------------------------------------------------
// nebula doc <tree.xml>:树 -> mermaid flowchart(自动流程文档)
// 只依赖 XML 结构,不要求节点已注册——评审/交接时给树"拍快照"。
// ---------------------------------------------------------------------------
static void docWalk(tinyxml2::XMLElement* el, const std::string& parentVar,
                    int* counter, std::ostringstream* out) {
  const std::string var = "n" + std::to_string((*counter)++);
  std::string label = el->Name();
  if (const char* name = el->Attribute("name")) {
    label += " : ";
    label += name;
  }
  // 转义 mermaid 标签中的引号与方括号
  for (auto& ch : label) {
    if (ch == '"' || ch == '[' || ch == ']') ch = '\'';
  }
  *out << "  " << var << "[\"" << label << "\"]\n";
  if (!parentVar.empty()) {
    *out << "  " << parentVar << " --> " << var << "\n";
  }
  for (tinyxml2::XMLElement* child = el->FirstChildElement(); child;
       child = child->NextSiblingElement()) {
    docWalk(child, var, counter, out);
  }
}

static int cmdDoc(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: nebula doc <tree.xml>\n";
    return 2;
  }
  tinyxml2::XMLDocument doc;
  if (doc.LoadFile(argv[2]) != tinyxml2::XML_SUCCESS) {
    std::cerr << "error: " << doc.ErrorStr() << "\n";
    return 2;
  }
  tinyxml2::XMLElement* root = doc.RootElement();
  if (!root) {
    std::cerr << "error: empty xml\n";
    return 2;
  }
  tinyxml2::XMLElement* bt = root->FirstChildElement("BehaviorTree");
  if (!bt) {
    std::cerr << "error: no <BehaviorTree> element\n";
    return 2;
  }
  std::ostringstream out;
  out << "flowchart TD\n";
  int counter = 0;
  for (tinyxml2::XMLElement* top = bt->FirstChildElement(); top;
       top = top->NextSiblingElement()) {
    docWalk(top, "", &counter, &out);
  }
  std::cout << out.str();
  return 0;
}

// ---------------------------------------------------------------------------
// nebula explain <journal.jsonl> [task_id]:任务事后分析
// 时间线 + 检查点 + 终态原因;"这单为什么走了这条路"的命令行入口。
// ---------------------------------------------------------------------------
static double journalSeconds(const std::string& ts) {
  // "YYYY-MM-DD HH:MM:SS.mmm" -> 当日秒数(同一任务的相对比较够用)
  int Y, M, D, h, m, s, ms = 0;
  if (std::sscanf(ts.c_str(), "%d-%d-%d %d:%d:%d.%d", &Y, &M, &D, &h, &m, &s,
                  &ms) < 6) {
    return 0.0;
  }
  return h * 3600.0 + m * 60.0 + s + ms / 1000.0;
}

static int cmdExplain(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: nebula explain <journal.jsonl> [task_id]\n";
    return 2;
  }
  std::ifstream f(argv[2]);
  if (!f) {
    std::cerr << "error: cannot read " << argv[2] << "\n";
    return 2;
  }
  const std::string filter = argc >= 4 ? argv[3] : "";

  // 收集每个任务的事件(保序)
  std::vector<nebulabt::JournalEvent> all;
  std::vector<std::string> taskOrder;
  std::map<std::string, std::vector<nebulabt::JournalEvent>> byTask;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    try {
      nebulabt::JournalEvent e =
          nlohmann::json::parse(line).get<nebulabt::JournalEvent>();
      if (e.task_id.empty()) continue;
      if (!filter.empty() && e.task_id != filter) continue;
      if (!byTask.count(e.task_id)) taskOrder.push_back(e.task_id);
      byTask[e.task_id].push_back(std::move(e));
    } catch (const std::exception&) {
      // 容错跳过(崩溃瞬间的不完整行)
    }
  }
  if (taskOrder.empty()) {
    std::cout << "no events found"
              << (filter.empty() ? "" : " for task " + filter) << "\n";
    return 1;
  }

  // 默认只展示最后一个任务(排查焦点);传 task_id 可指定
  const std::string& taskId = taskOrder.back();
  const auto& events = byTask[taskId];

  std::string taskType, pack, packVersion, state, reason;
  double t0 = -1, t1 = -1;
  std::cout << "task " << taskId << "\n";
  for (const auto& e : events) {
    taskType = e.task_type.empty() ? taskType : e.task_type;
    if (e.data.contains("pack") && e.data["pack"].is_string()) {
      pack = e.data["pack"].get<std::string>();
      packVersion = e.data.value("pack_version", std::string());
    }
    const double sec = journalSeconds(e.timestamp);
    if (t0 < 0) t0 = sec;
    t1 = sec;
    std::cout << "  [" << e.timestamp << "] " << e.event;
    if (e.event == "checkpoint" || e.event == "checkpoint_restored") {
      std::cout << " " << e.data.value("id", std::string());
    }
    if (e.event == "finished") {
      state = e.data.value("state", std::string());
      reason = e.data.value("reason", std::string());
      std::cout << " " << state << (reason.empty() ? "" : " (" + reason + ")");
    }
    std::cout << "\n";
  }
  std::cout << "task_type: " << taskType;
  if (!pack.empty()) {
    std::cout << ", pack: " << pack << " v" << packVersion;
  }
  if (state == "SUCCEEDED" || state == "FAILED" || state == "CANCELED") {
    std::cout << "\nduration: " << (t1 - t0) << " s, state: " << state
              << (reason.empty() ? "" : " (" + reason + ")");
  } else {
    std::cout << "\n(no terminal event — task likely crashed mid-run; "
                 "resume via TaskSpec.resume_journal)";
  }
  std::cout << "\n";
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: nebula <command> [args]\n"
              << "commands:\n"
              << "  validate <tree.xml> [schema.json]\n"
              << "  journal  <file.jsonl>\n"
              << "  doc      <tree.xml>\n"
              << "  explain  <journal.jsonl> [task_id]\n";
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "validate") {
    return cmdValidate(argc, argv);
  }
  if (cmd == "journal") {
    return cmdJournal(argc, argv);
  }
  if (cmd == "doc") {
    return cmdDoc(argc, argv);
  }
  if (cmd == "explain") {
    return cmdExplain(argc, argv);
  }
  std::cerr << "error: unknown command '" << cmd << "'\n";
  return 2;
}
