#include "nebulabt/core/pack.hpp"

#include <stdexcept>

namespace nebulabt {

namespace {
const nlohmann::json& requireObject(const nlohmann::json& j,
                                    const char* field) {
  auto it = j.find(field);
  if (it == j.end() || !it->is_object()) {
    throw std::runtime_error(std::string("pack manifest: '") + field +
                             "' must be an object");
  }
  return *it;
}
} // namespace

PackManifest parsePackManifest(const nlohmann::json& j) {
  if (!j.is_object()) {
    throw std::runtime_error("pack manifest must be a JSON object");
  }
  PackManifest m;
  auto name = j.find("name");
  auto version = j.find("version");
  if (name == j.end() || !name->is_string() || name->get<std::string>().empty()) {
    throw std::runtime_error("pack manifest: missing 'name'");
  }
  if (version == j.end() || !version->is_string() ||
      version->get<std::string>().empty()) {
    throw std::runtime_error("pack manifest: missing 'version'");
  }
  m.name = name->get<std::string>();
  m.version = version->get<std::string>();
  if (auto dir = j.find("trees_dir");
      dir != j.end() && dir->is_string() && !dir->get<std::string>().empty()) {
    m.trees_dir = dir->get<std::string>();
  }

  const auto& tasks = requireObject(j, "task_types");
  for (auto it = tasks.begin(); it != tasks.end(); ++it) {
    if (!it.value().is_object()) {
      throw std::runtime_error("pack manifest: task '" + it.key() +
                               "' must be an object");
    }
    PackTaskSpec spec;
    auto tree = it.value().find("tree");
    if (tree == it.value().end() || !tree->is_string() ||
        tree->get<std::string>().empty()) {
      throw std::runtime_error("pack manifest: task '" + it.key() +
                               "' missing 'tree'");
    }
    spec.tree = tree->get<std::string>();
    if (auto schema = it.value().find("schema");
        schema != it.value().end() && schema->is_object()) {
      spec.schema = *schema;
    }
    if (auto timeout = it.value().find("timeout_ms");
        timeout != it.value().end() && timeout->is_number_unsigned()) {
      spec.timeout_ms = timeout->get<uint64_t>();
    }
    m.tasks[it.key()] = std::move(spec);
  }
  if (m.tasks.empty()) {
    throw std::runtime_error("pack manifest: empty 'task_types'");
  }
  return m;
}

} // namespace nebulabt
