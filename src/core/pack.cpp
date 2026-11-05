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

namespace {
// 解析单个 task/template 条目(tree + 可选 schema/timeout/slot_defaults)
PackTaskSpec parseTaskEntry(const std::string& key, const nlohmann::json& v) {
  if (!v.is_object()) {
    throw std::runtime_error("pack manifest: entry '" + key +
                             "' must be an object");
  }
  PackTaskSpec spec;
  auto tree = v.find("tree");
  if (tree != v.end() && tree->is_string() && !tree->get<std::string>().empty()) {
    spec.tree = tree->get<std::string>();
  }
  if (auto tpl = v.find("template");
      tpl != v.end() && tpl->is_string() && !tpl->get<std::string>().empty()) {
    spec.template_name = tpl->get<std::string>();
  }
  if (spec.tree.empty() && spec.template_name.empty()) {
    throw std::runtime_error("pack manifest: entry '" + key +
                             "' needs 'tree' or 'template'");
  }
  if (auto schema = v.find("schema");
      schema != v.end() && schema->is_object()) {
    spec.schema = *schema;
  }
  if (auto defaults = v.find("slot_defaults");
      defaults != v.end() && defaults->is_object()) {
    spec.slot_defaults = *defaults;
  }
  if (auto timeout = v.find("timeout_ms");
      timeout != v.end() && timeout->is_number_unsigned()) {
    spec.timeout_ms = timeout->get<uint64_t>();
  }
  if (auto codes = v.find("failure_codes");
      codes != v.end() && codes->is_object()) {
    for (auto it = codes->begin(); it != codes->end(); ++it) {
      if (!it.value().is_string()) {
        throw std::runtime_error("pack manifest: entry '" + key +
                                 "' failure_codes." + it.key() +
                                 "' must be a string description");
      }
      spec.failure_codes[it.key()] = it.value().get<std::string>();
    }
  }
  return spec;
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

  // 模板(可选)
  if (auto tpls = j.find("templates"); tpls != j.end() && tpls->is_object()) {
    for (auto it = tpls->begin(); it != tpls->end(); ++it) {
      m.templates[it.key()] = parseTaskEntry(it.key(), it.value());
      if (m.templates[it.key()].tree.empty()) {
        throw std::runtime_error("pack manifest: template '" + it.key() +
                                 "' missing 'tree'");
      }
    }
  }

  // 任务(必填);引用模板时继承 tree/schema/timeout
  const auto& tasks = requireObject(j, "task_types");
  for (auto it = tasks.begin(); it != tasks.end(); ++it) {
    PackTaskSpec spec = parseTaskEntry(it.key(), it.value());
    if (!spec.template_name.empty()) {
      auto tpl = m.templates.find(spec.template_name);
      if (tpl == m.templates.end()) {
        throw std::runtime_error("pack manifest: task '" + it.key() +
                                 "' references unknown template '" +
                                 spec.template_name + "'");
      }
      if (spec.tree.empty()) spec.tree = tpl->second.tree;
      if (spec.schema.empty()) spec.schema = tpl->second.schema;
      if (spec.timeout_ms == 0) spec.timeout_ms = tpl->second.timeout_ms;
      spec.template_name = "";  // 解析完成,下游只看结果
    }
    if (spec.tree.empty()) {
      throw std::runtime_error("pack manifest: task '" + it.key() +
                               "' has no tree");
    }
    m.tasks[it.key()] = std::move(spec);
  }
  if (m.tasks.empty()) {
    throw std::runtime_error("pack manifest: empty 'task_types'");
  }

  // requires(可选)
  if (auto reqs = j.find("requires"); reqs != j.end() && reqs->is_array()) {
    for (const auto& r : *reqs) {
      if (!r.is_object()) continue;
      PackManifest::Require req;
      if (auto p = r.find("pack");
          p != r.end() && p->is_string() && !p->get<std::string>().empty()) {
        req.pack = p->get<std::string>();
      } else {
        throw std::runtime_error("pack manifest: requires entry missing 'pack'");
      }
      if (auto mv = r.find("min_version"); mv != r.end() && mv->is_string()) {
        req.min_version = mv->get<std::string>();
      }
      m.requires.push_back(std::move(req));
    }
  }
  return m;
}

} // namespace nebulabt
