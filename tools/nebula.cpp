// nebula CLI — NebulaBT 命令行工具
//
// 用法:
//   nebula validate <tree.xml> [schema.json]
//     加载期校验:树可解析、节点已注册、端口引用的黑板键可溯源
//     (schema 声明 / 框架注入 / 树内 Output 写出)。供 CI 门禁使用。
//   nebula journal <file>
//     检查持久化的 journal(帧格式或裸 JSONL,自适应):解析容错统计、
//     事件类型分布、检查点清单——恢复前排查用。
//   nebula replay <journal-file> [--until N]
//     恢复态预演(M6):给定 journal,回答"恢复重跑会发生什么"——
//     哪些检查点会被跳过、恢复哪些黑板键、上次运行终态。不执行树,
//     无需注册业务动作;--until N 只考虑前 N 个事件(时间旅行点)。
//   nebula explain <journal-file> [task_id]
//     还原任务执行轨迹:时间线 + 节点迁移 + 失败记录。
//   nebula doc <tree.xml>
//     树 -> mermaid 流程图(自动流程文档)。

#include <nebulabt/core/engine.hpp>
#include <nebulabt/core/journal.hpp>
#include <nebulabt/core/journal_file.hpp>

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

// 读取 journal 事件:优先帧格式(M4 JournalFileWriter 落盘),退回裸 JSONL。
// 返回可解析事件;bad 记入 badCount(帧格式下 torn_tail 也计入)
static std::vector<nebulabt::JournalEvent> loadJournalAuto(
    const std::string& path, int* okCount, int* badCount,
    std::string* tornNote = nullptr) {
  std::vector<nebulabt::JournalEvent> out;
  *okCount = 0;
  *badCount = 0;
  nlohmann::json arr;
  nebulabt::JournalFileReadStats stats;
  std::string err;
  if (nebulabt::readJournalFile(path, arr, &stats, &err) &&
      stats.good_records > 0) {
    for (const auto& e : arr) {
      try {
        out.push_back(e.get<nebulabt::JournalEvent>());
        ++(*okCount);
      } catch (const std::exception&) {
        ++(*badCount);
      }
    }
    if (stats.torn_tail && tornNote) {
      *tornNote = "torn tail discarded (power-loss window)";
    }
    return out;
  }
  // 裸 JSONL
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    try {
      out.push_back(
          nlohmann::json::parse(line).get<nebulabt::JournalEvent>());
      ++(*okCount);
    } catch (const std::exception&) {
      ++(*badCount);
    }
  }
  return out;
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

// nebula journal <file>:检查持久化 journal(恢复前排查;帧格式/JSONL 自适应)
static int cmdJournal(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: nebula journal <file>\n";
    return 2;
  }
  int ok = 0, bad = 0;
  std::string tornNote;
  const auto events = loadJournalAuto(argv[2], &ok, &bad, &tornNote);

  int tasks = 0;
  std::string lastTask;
  std::map<std::string, int> byEvent;
  std::map<std::string, std::string> lastState;  // task_id -> finished state
  std::map<std::string, std::string> lastType;
  std::vector<std::string> checkpoints;

  for (const auto& e : events) {
    if (e.task_id.empty() || e.event.empty()) {
      ++bad;
      continue;
    }
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
  }
  (void)ok;

  std::cout << "file: " << argv[2] << "\n"
            << "events ok: " << events.size() << ", bad: " << bad << "\n";
  if (!tornNote.empty()) {
    std::cout << "note: " << tornNote << "\n";
  }
  std::cout << "tasks: " << tasks << "\n";
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

// ============================================================================
// nebula replay — 恢复态预演(M6)
//
// 读 journal(帧格式,兼容裸 JSONL),回答"恢复重跑会发生什么":
//   - 哪些 Checkpoint 会被跳过(已完成段,副作用不重复执行)
//   - 每个跳过段恢复哪些黑板键
//   - 上一次运行的终态与原因
// 不执行树(无需注册业务动作),纯 journal 分析,恢复前排查用。
// ============================================================================
static int cmdReplay(int argc, char** argv) {
  long until = -1;
  std::string path;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--until" && i + 1 < argc) {
      until = std::atol(argv[++i]);
    } else if (path.empty()) {
      path = a;
    } else {
      std::cerr << "usage: nebula replay <journal-file> [--until N]\n";
      return 2;
    }
  }
  if (path.empty()) {
    std::cerr << "usage: nebula replay <journal-file> [--until N]\n";
    return 2;
  }

  nlohmann::json events;
  nebulabt::JournalFileReadStats stats;
  std::string err;
  const bool frameOk =
      nebulabt::readJournalFile(path, events, &stats, &err);
  if (!frameOk) {
    // 兼容裸 JSONL(逐行 dump 的旧格式)
    std::ifstream f(path);
    if (!f) {
      std::cerr << "error: cannot read " << path << "\n";
      return 2;
    }
    events = nlohmann::json::array();
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty()) continue;
      try {
        events.push_back(nlohmann::json::parse(line));
      } catch (const std::exception&) {
      }
    }
    stats.good_records = events.size();
  }
  if (until >= 0 && static_cast<size_t>(until) < events.size()) {
    events.erase(events.begin() + until, events.end());
  }
  if (events.empty()) {
    std::cout << "no events to replay\n";
    return 1;
  }

  std::string taskId, taskType;
  std::string lastState, lastReason;
  // 检查点 -> 最近一次快照(引擎恢复语义:同名 id 后写覆盖)
  std::map<std::string, nlohmann::json> checkpoints;
  for (const auto& e : events) {
    if (!e.is_object()) continue;
    const std::string ev = e.value("event", "");
    if (ev == "submitted") {
      taskId = e.value("task_id", "");
      taskType = e.value("task_type", "");
    } else if (ev == "checkpoint" && e.contains("data") &&
               e["data"].contains("id")) {
      checkpoints[e["data"]["id"].get<std::string>()] =
          e["data"].value("blackboard", nlohmann::json::object());
    } else if (ev == "finished") {
      lastState = e["data"].value("state", "");
      lastReason = e["data"].value("reason", "");
    }
  }

  std::cout << "replay preview: " << path << "\n";
  std::cout << "  events considered : " << events.size() << " / "
            << stats.good_records << " good records";
  if (stats.torn_tail) {
    std::cout << "  (torn tail detected: tail record discarded)";
  }
  std::cout << "\n  task              : " << taskId << " (" << taskType
            << ")\n";
  std::cout << "  last finished     : " << (lastState.empty() ? "(none)"
                                                           : lastState);
  if (!lastReason.empty()) {
    std::cout << " (" << lastReason << ")";
  }
  std::cout << "\n";

  std::cout << "  resume would skip : ";
  if (checkpoints.empty()) {
    std::cout << "(no completed checkpoints — full re-run)\n";
  } else {
    std::cout << checkpoints.size() << " checkpoint(s):\n";
    for (const auto& [id, snap] : checkpoints) {
      std::cout << "    - " << id << "  restores blackboard keys:";
      if (snap.is_object()) {
        for (auto it = snap.begin(); it != snap.end(); ++it) {
          std::cout << " " << it.key();
        }
      }
      if (snap.is_object() && snap.empty()) {
        std::cout << " (none)";
      }
      std::cout << "\n";
    }
  }
  std::cout << "  note: segments after the last checkpoint re-execute;\n"
               "        business actions must be idempotent (see DESIGN 4.7)\n";
  return 0;
}

static int cmdExplain(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: nebula explain <journal-file> [task_id]\n";
    return 2;
  }
  int okCount = 0, badCount = 0;
  const auto all = loadJournalAuto(argv[2], &okCount, &badCount);
  const std::string filter = argc >= 4 ? argv[3] : "";

  // 收集每个任务的事件(保序)
  std::vector<std::string> taskOrder;
  std::map<std::string, std::vector<nebulabt::JournalEvent>> byTask;
  for (auto& e : all) {
    if (e.task_id.empty()) continue;
    if (!filter.empty() && e.task_id != filter) continue;
    if (!byTask.count(e.task_id)) taskOrder.push_back(e.task_id);
    byTask[e.task_id].push_back(std::move(e));
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
              << "  explain  <journal.jsonl> [task_id]\n"
              << "  replay   <journal-file> [--until N]\n";
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
  if (cmd == "replay") {
    return cmdReplay(argc, argv);
  }
  std::cerr << "error: unknown command '" << cmd << "'\n";
  return 2;
}
