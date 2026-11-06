// ============================================================================
// nebula CLI 冒烟测试(测试加固批次)
//
// 直接调用 nebula 二进制(路径由 CMake 注入),断言各子命令的
// 退出码与关键输出。覆盖:validate(合法/非法/--json)、journal、
// replay(帧格式 + --until)、doc。
// ============================================================================

#include <nebulabt/core/journal_file.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "CHECK failed: " #cond " (line " << __LINE__ << ")\n";      \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

static const std::string kNebula = NEBULA_BIN;

static bool writeFile(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << content;
  return static_cast<bool>(f);
}

// 执行子命令,返回 {exit_code, output}
static std::pair<int, std::string> run(const std::string& args) {
  const std::string outPath = "/tmp/nebulabt_cli_out.txt";
  const int rc = std::system(
      (kNebula + " " + args + " > " + outPath + " 2>&1").c_str());
  std::ifstream f(outPath);
  std::string out((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  return {rc == -1 ? 127 : WEXITSTATUS(rc), out};
}

int main() {
  namespace fs = std::filesystem;
  const std::string tmp = "/tmp/nebulabt_cli_test";
  fs::create_directories(tmp);

  // ---- validate:合法树(仅内置节点,无需注册动作)----
  const std::string goodXml = tmp + "/good.xml";
  CHECK(writeFile(goodXml, R"(
<root BTCPP_format="v4" main_tree_to_execute="t">
  <BehaviorTree ID="t">
    <Sequence>
      <AlwaysSuccess/>
      <AlwaysSuccess/>
    </Sequence>
  </BehaviorTree>
</root>)"));
  {
    const auto [rc, out] = run("validate " + goodXml);
    CHECK(rc == 0);
    CHECK(out.find("OK") != std::string::npos);
  }
  // --json 合法
  {
    const auto [rc, out] = run("validate " + goodXml + " --json");
    CHECK(rc == 0);
    CHECK(out.find("\"ok\":true") != std::string::npos);
  }

  // ---- validate:非法树(未知节点)→ 退出码 1 + 错误清单 ----
  const std::string badXml = tmp + "/bad.xml";
  CHECK(writeFile(badXml, R"(
<root BTCPP_format="v4"><BehaviorTree ID="t"><NoSuchNode_XYZ/></BehaviorTree></root>)"));
  {
    const auto [rc, out] = run("validate " + badXml);
    CHECK(rc == 1);
    CHECK(out.find("error") != std::string::npos);
  }
  // --json 非法:机器可读错误回灌(M8 护栏闭环)
  {
    const auto [rc, out] = run("validate " + badXml + " --json");
    CHECK(rc == 1);
    CHECK(out.find("\"ok\":false") != std::string::npos);
    CHECK(out.find("errors") != std::string::npos);
  }

  // ---- journal:裸 JSONL ----
  const std::string jsonl = tmp + "/events.jsonl";
  CHECK(writeFile(jsonl,
                  "{\"timestamp\":\"2026-11-06 10:00:00.000\",\"task_id\":\"t1\","
                  "\"task_type\":\"DEMO\",\"event\":\"submitted\",\"data\":{}}\n"
                  "{\"timestamp\":\"2026-11-06 10:00:00.010\",\"task_id\":\"t1\","
                  "\"task_type\":\"DEMO\",\"event\":\"finished\",\"data\":{\"state\":\"SUCCEEDED\",\"reason\":\"\"}}\n"));
  {
    const auto [rc, out] = run("journal " + jsonl);
    CHECK(rc == 0);
    CHECK(out.find("t1") != std::string::npos);
  }

  // ---- replay:帧格式 journal(含检查点)+ --until ----
  const std::string framed = tmp + "/events.bin";
  {
    nebulabt::JournalFileWriter w;
    CHECK(w.open(framed, false));
    nebulabt::JournalEvent e;
    e.task_id = "t1"; e.task_type = "DEMO";
    e.event = "submitted";
    CHECK(w.append(e));
    e.event = "checkpoint";
    e.data = nlohmann::json{{"id", "cp1"},
                            {"blackboard", nlohmann::json{{"sku", "a1"}}}};
    CHECK(w.append(e));
    e.event = "finished";
    e.data = nlohmann::json{{"state", "FAILED"}, {"reason", "ship failed"}};
    CHECK(w.append(e));
    // 掉电窗口:撕裂尾帧,恢复侧必须容错
    std::ofstream f(framed, std::ios::app | std::ios::binary);
    const char torn[] = {0x40, 0x00, 0x00, 0x00, 0x11};
    f.write(torn, sizeof(torn));
  }
  {
    const auto [rc, out] = run("replay " + framed);
    CHECK(rc == 0);
    CHECK(out.find("replay preview") != std::string::npos);
    CHECK(out.find("cp1") != std::string::npos);           // 恢复会跳过
    CHECK(out.find("FAILED") != std::string::npos);        // 上轮终态
    CHECK(out.find("torn tail") != std::string::npos);     // 撕裂标记
  }
  {
    const auto [rc, out] = run("replay " + framed + " --until 1");
    CHECK(rc == 0);
    CHECK(out.find("(no completed checkpoints") != std::string::npos);  // 截断后无检查点
  }

  // ---- doc:树 → mermaid ----
  {
    const auto [rc, out] = run("doc " + goodXml);
    CHECK(rc == 0);
    CHECK(out.find("flowchart TD") != std::string::npos);
  }

  std::cout << "ALL CLI SMOKE TESTS PASSED\n";
  fs::remove_all(tmp);
  return 0;
}
