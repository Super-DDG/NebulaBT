// nebula CLI — NebulaBT 命令行工具
//
// 用法:
//   nebula validate <tree.xml> [schema.json]
//     加载期校验:树可解析、节点已注册、端口引用的黑板键可溯源
//     (schema 声明 / 框架注入 / 树内 Output 写出)。供 CI 门禁使用。
//
// 规划中的子命令:explain / replay / doc(见 docs/DESIGN.md 路线图)

#include <nebulabt/core/engine.hpp>

#include <fstream>
#include <iostream>
#include <sstream>

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

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: nebula <command> [args]\n"
              << "commands:\n"
              << "  validate <tree.xml> [schema.json]\n";
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "validate") {
    return cmdValidate(argc, argv);
  }
  std::cerr << "error: unknown command '" << cmd << "'\n";
  return 2;
}
