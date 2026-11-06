#include "manifest_io.hpp"

#include <fstream>
#include <sstream>

#ifdef NEBULABT_HAVE_YAML
#include <yaml-cpp/yaml.h>
#endif

namespace nebulabt {
namespace detail {

#ifdef NEBULABT_HAVE_YAML

// YAML 节点 -> nlohmann::json(标量尽力保真:整数/浮点/布尔/字符串;
// 引号标量恒为字符串,数字判定要求整串精确匹配,避免 "1.0.0" 被吞成 1.0)
static nlohmann::json yamlToJson(const YAML::Node& node) {
  switch (node.Type()) {
    case YAML::NodeType::Null:
      return nullptr;
    case YAML::NodeType::Scalar: {
      const std::string& s = node.Scalar();
      if (node.Tag() == "!!str") {
        return s;  // 引号标量:恒为字符串
      }
      if (s == "true" || s == "True") return true;
      if (s == "false" || s == "False") return false;
      if (s == "null" || s == "~" || s.empty()) return nullptr;
      // 整数:全数字(允许符号)
      {
        size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
        if (i < s.size()) {
          bool allDigits = true;
          for (size_t j = i; j < s.size(); ++j) {
            if (s[j] < '0' || s[j] > '9') {
              allDigits = false;
              break;
            }
          }
          if (allDigits) {
            try {
              return nlohmann::json(std::stoll(s));
            } catch (const std::exception&) {
            }
          }
        }
      }
      // 浮点:含 . 或 e/E 且 round-trip 一致(排除版本号 "1.0.0" 之类)
      if (s.find('.') != std::string::npos ||
          s.find('e') != std::string::npos ||
          s.find('E') != std::string::npos) {
        try {
          const double d = std::stod(s);
          std::ostringstream back;
          back << d;
          if (back.str() == s) {
            return nlohmann::json(d);
          }
        } catch (const std::exception&) {
        }
      }
      return s;
    }
    case YAML::NodeType::Sequence: {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& e : node) arr.push_back(yamlToJson(e));
      return arr;
    }
    case YAML::NodeType::Map: {
      nlohmann::json obj = nlohmann::json::object();
      for (const auto& kv : node) obj[kv.first.as<std::string>()] = yamlToJson(kv.second);
      return obj;
    }
    default:
      return nullptr;
  }
}

#endif

bool readManifestFile(const std::string& dir, nlohmann::json* out,
                      std::string* out_format, std::string* error) {
  const std::string jsonPath = dir + "/pack.json";
  {
    std::ifstream f(jsonPath);
    if (f) {
      try {
        f >> *out;
      } catch (const std::exception& e) {
        if (error) *error = "pack.json parse: " + std::string(e.what());
        return false;
      }
      if (out_format) *out_format = "json";
      return true;
    }
  }

#ifdef NEBULABT_HAVE_YAML
  const std::string yamlPath = dir + "/pack.yaml";
  {
    std::ifstream f(yamlPath);
    if (f) {
      try {
        *out = yamlToJson(YAML::LoadFile(yamlPath));
      } catch (const std::exception& e) {
        if (error) *error = "pack.yaml parse: " + std::string(e.what());
        return false;
      }
      if (out_format) *out_format = "yaml";
      return true;
    }
  }
#else
  (void)out_format;
#endif

  if (error) {
#ifdef NEBULABT_HAVE_YAML
    *error = "cannot read " + dir + "/pack.json or pack.yaml";
#else
    *error = "cannot read " + dir + "/pack.json (pack.yaml requires "
             "-DNEBULABT_WITH_YAML=ON and yaml-cpp)";
#endif
  }
  return false;
}

} // namespace detail
} // namespace nebulabt
