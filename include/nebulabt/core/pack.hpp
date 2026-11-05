#pragma once
/// Tree Pack(M3):树是带版本的业务资产。
///
/// 一个 pack = 一个目录:
///   pack.json                 # 清单(见下)
///   trees/*.xml               # 树文件
///
/// pack.json 格式:
/// {
///   "name": "delivery",
///   "version": "1.2.0",
///   "trees_dir": "trees",                  // 可选,默认 "trees"
///   "requires": [ {"pack": "common", "min_version": "1.0.0"} ],  // 可选
///   "templates": {                         // 可选:模板树(变体不 fork 的基础)
///     "deliver": { "tree": "deliver_item", "schema": {...}, "timeout_ms": 60000 }
///   },
///   "task_types": {
///     "DELIVER_ORDER": {
///       "tree": "deliver_item",            // 直接声明,或用 "template" 引用
///       "schema": { "slots": { ... } },    // 可选;引用模板时缺省继承
///       "timeout_ms": 60000,               // 可选;引用模板时缺省继承
///       "slot_defaults": {"grasp_retry": 3} // 可选:实例默认 slots(提交未给时生效)
///     }
///   }
/// }
///
/// 模板 + slot_defaults 解决"变体 fork":同一棵树,不同任务类型给不同
/// 默认参数,不再整棵复制 XML。
///
/// requires:loadPack 时校验依赖 pack 已加载且版本 >= min_version,
/// 让 pack 拆分有依赖顺序保障。
///
/// 加载即校验(loadPack):每棵树必须可解析、schema 数据流检查通过——
/// CI 里加载全部 pack 就是最强门禁。同名同版本重复加载抛异常。
///
/// 灰度路由(setCanary):同一 pack 可加载多个版本;
/// task_id 哈希 % 100 < percent 的任务路由到 canary 版本,其余走最高版本;
/// TaskSpec.pack_version 可显式钉住。

#include "nebulabt/core/task.hpp"
#include <behaviortree_cpp/contrib/json.hpp>
#include <map>
#include <string>
#include <vector>

namespace nebulabt {

struct PackTaskSpec {
  std::string tree;
  std::string template_name;  // 引用的模板(空 = 直接声明)
  nlohmann::json schema = nlohmann::json::object();
  nlohmann::json slot_defaults = nlohmann::json::object();  // 实例默认 slots
  uint64_t timeout_ms = 0;
};

struct PackManifest {
  std::string name;
  std::string version;
  std::string trees_dir = "trees";
  std::map<std::string, PackTaskSpec> templates;
  std::map<std::string, PackTaskSpec> tasks;
  struct Require {
    std::string pack;
    std::string min_version;
  };
  std::vector<Require> requires;
};

/// 解析清单;格式错误抛 std::runtime_error(带字段上下文)
PackManifest parsePackManifest(const nlohmann::json& j);

} // namespace nebulabt
