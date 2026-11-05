#pragma once
/// L2 校验(树 ↔ schema 数据流检查)与 slots 注入(engine 内部)。

#include "nebulabt/core/schema.hpp"
#include "nebulabt/core/task.hpp"
#include <behaviortree_cpp/basic_types.h>
#include <map>
#include <string>
#include <vector>

namespace BT {
class Tree;
}

namespace nebulabt {
namespace detail {

/// 收集树中所有端口引用的黑板键({key} 形式),要求每个"被读"的键属于:
///   schema 声明 / 框架注入(task_id, task_type)/ 树内某节点 Output 写出
/// 目标:键名拼写错误、忘记声明 slots 这类问题在加载期暴露,
/// 而不是运行时表现为"注入了但没人读"的空串。
void validateTreeKeys(const BT::Tree& tree,
                      const std::map<std::string, SlotDef>& defs,
                      std::vector<std::string>& errors);

/// 提交前注入:task_id/task_type + slots(按 schema 类型真实注入)
void injectSlots(BT::Tree& tree, const TaskSpec& spec,
                 const std::map<std::string, SlotDef>& defs);

} // namespace detail
} // namespace nebulabt
