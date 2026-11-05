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
///   "task_types": {
///     "DELIVER_ORDER": {
///       "tree": "deliver_item",            // trees_dir 下不含 .xml
///       "schema": { "slots": { ... } },    // 可选,M1 格式
///       "timeout_ms": 60000                // 可选,默认策略
///     }
///   }
/// }
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
  nlohmann::json schema = nlohmann::json::object();
  uint64_t timeout_ms = 0;
};

struct PackManifest {
  std::string name;
  std::string version;
  std::string trees_dir = "trees";
  std::map<std::string, PackTaskSpec> tasks;
};

/// 解析清单;格式错误抛 std::runtime_error(带字段上下文)
PackManifest parsePackManifest(const nlohmann::json& j);

} // namespace nebulabt
