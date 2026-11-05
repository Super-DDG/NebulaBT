#pragma once
/// BT::Any -> nlohmann::json 的安全转换。
/// 注意:黑板值类型必须按 Any::type() 精确分发——bb->get<T> 对类型不匹配
/// 会抛 Any::convert 异常而不是返回 false(见 ADR 与 DESIGN 教训记录)。
///
/// 模板参数为 BT::LockedPtr<Any> 等可 -> 解引用的持有者。

#include <behaviortree_cpp/basic_types.h>
#include <behaviortree_cpp/contrib/json.hpp>
#include <string>
#include <vector>

namespace nebulabt {

template <typename LockedAny>
nlohmann::json anyToJson(LockedAny& entry) {
  const std::type_index t = entry->type();
  try {
    if (t == typeid(std::string)) {
      return entry->template cast<std::string>();
    }
    if (t == typeid(int)) {
      return entry->template cast<int>();
    }
    if (t == typeid(int64_t)) {
      return entry->template cast<int64_t>();
    }
    if (t == typeid(double)) {
      return entry->template cast<double>();
    }
    if (t == typeid(bool)) {
      return entry->template cast<bool>();
    }
    if (t == typeid(std::vector<std::string>)) {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& s : entry->template cast<std::vector<std::string>>()) {
        arr.push_back(s);
      }
      return arr;
    }
  } catch (const std::exception&) {
    // 不支持的类型返回 null,调用方自行跳过
  }
  return {};
}

} // namespace nebulabt
