#pragma once
/// 任务 slots schema(M1):加载期校验,让错误在加载期爆,不在深夜爆。
///
/// schema JSON 格式:
/// {
///   "slots": {
///     "target_item": {"type": "string", "required": true},
///     "grasp_retry": {"type": "int",    "required": false}
///   }
/// }
/// 支持类型:string / int / double / bool
///
/// 两层校验(均发生在加载期):
///   L1 提交 ↔ schema:required 缺失 / 未知字段(防拼写错)/ 类型不符
///   L2 树 ↔ schema:树中端口引用的黑板键,必须是 schema 声明的键、
///      框架注入键(task_id/task_type)或树内某节点 Output 端口写出的键
///      ——即"端口级数据流检查",中间键由树自己声明来源
///
/// 未提供 schema(空对象)= 关闭校验,完全向后兼容。

#include <behaviortree_cpp/contrib/json.hpp>
#include <map>
#include <string>
#include <vector>

namespace nebulabt {

struct SlotDef {
  std::string type;
  bool required = false;
};

/// 解析 schema;格式问题记入 errors(跳过坏条目,不抛异常)
std::map<std::string, SlotDef> parseSlotSchema(const nlohmann::json& schema,
                                               std::vector<std::string>& errors);

bool jsonMatchesType(const nlohmann::json& v, const std::string& type);

/// L1:提交 slots ↔ schema;defs 为空时直接通过(未启用校验)
std::vector<std::string> validateSlotsAgainstSchema(
    const std::map<std::string, SlotDef>& defs, const nlohmann::json& slots);

} // namespace nebulabt
