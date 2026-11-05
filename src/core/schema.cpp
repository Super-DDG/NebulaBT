#include "nebulabt/core/schema.hpp"

namespace nebulabt {

bool jsonMatchesType(const nlohmann::json& v, const std::string& type) {
  if (type == "string") return v.is_string();
  if (type == "int") return v.is_number_integer();
  if (type == "double") return v.is_number();
  if (type == "bool") return v.is_boolean();
  if (type == "string_array") {
    if (!v.is_array()) return false;
    for (const auto& e : v) {
      if (!e.is_string()) return false;
    }
    return true;
  }
  return false;
}

std::map<std::string, SlotDef> parseSlotSchema(const nlohmann::json& schema,
                                               std::vector<std::string>& errors) {
  std::map<std::string, SlotDef> defs;
  if (!schema.is_object()) {
    return defs;
  }
  const auto slots = schema.find("slots");
  if (slots == schema.end() || !slots->is_object()) {
    return defs;
  }
  for (auto it = slots->begin(); it != slots->end(); ++it) {
    const std::string& key = it.key();
    const auto& def = it.value();
    if (!def.is_object() || !def.contains("type") || !def["type"].is_string()) {
      errors.push_back("schema slot '" + key + "': missing string 'type'");
      continue;
    }
    SlotDef sd;
    sd.type = def["type"].get<std::string>();
    sd.required = def.value("required", false);
    if (sd.type != "string" && sd.type != "int" && sd.type != "double" &&
        sd.type != "bool" && sd.type != "string_array") {
      errors.push_back("schema slot '" + key + "': unknown type '" + sd.type +
                       "' (string|int|double|bool|string_array)");
      continue;
    }
    defs[key] = sd;
  }
  return defs;
}

std::vector<std::string> validateSlotsAgainstSchema(
    const std::map<std::string, SlotDef>& defs, const nlohmann::json& slots) {
  std::vector<std::string> errors;
  if (defs.empty()) {
    return errors;  // 未启用 schema 校验
  }
  if (!slots.is_object()) {
    errors.push_back("slots must be a JSON object");
    return errors;
  }
  for (const auto& [key, def] : defs) {
    if (slots.contains(key)) {
      if (!jsonMatchesType(slots[key], def.type)) {
        errors.push_back("slot '" + key + "': expected " + def.type);
      }
    } else if (def.required) {
      errors.push_back("required slot '" + key + "' is missing");
    }
  }
  // 未知字段一律拒绝:拼写错误在加载期暴露,而不是变成"注入了但没人读"
  for (auto it = slots.begin(); it != slots.end(); ++it) {
    if (!defs.count(it.key())) {
      errors.push_back("unknown slot '" + it.key() +
                       "' (not declared in schema)");
    }
  }
  return errors;
}

} // namespace nebulabt
