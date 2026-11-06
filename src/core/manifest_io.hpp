#pragma once
/// 树包清单读取(内部):pack.json(规范)或 pack.yaml(可选)。
/// YAML 需编译期启用(-DNEBULABT_WITH_YAML=ON 且系统有 yaml-cpp);
/// 二者字段结构完全一致,YAML 仅多做一层到 nlohmann::json 的转换。

#include <behaviortree_cpp/contrib/json.hpp>
#include <string>

namespace nebulabt {
namespace detail {

/// 读取 <dir>/pack.json;不存在时回落 <dir>/pack.yaml(仅 YAML 编译启用)。
/// 成功返回 true 并给出 manifest JSON 与格式说明;失败给出 error。
bool readManifestFile(const std::string& dir, nlohmann::json* out,
                      std::string* out_format, std::string* error);

} // namespace detail
} // namespace nebulabt
