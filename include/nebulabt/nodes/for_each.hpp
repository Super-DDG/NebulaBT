#pragma once
/// ForEach:动态树原语——针对"业务逻辑按数据集合展开"的场景。
/// (动机见 docs/DESIGN.md;BT.CPP 的 XML 树是静态的,集合遍历
///  一直是行为树进入业务领域的最大阻力之一。)
///
/// 遍历 array,每轮将当前元素写入 element 输出端口,依次 tick 子节点:
/// - 子节点 SUCCESS -> 处理下一个元素
/// - 子节点 RUNNING -> 返回 RUNNING(下轮从当前元素继续)
/// - 子节点 FAILURE -> 默认整体 FAILURE;continue_on_failure=true 时跳过继续
/// 空数组直接 SUCCESS。
///
/// array 支持(按黑板实际类型自动分发):
/// - vector<string>(兼容路径)
/// - nlohmann::json 数组(结构化黑板):字符串元素写 element(string);
///   对象/数值元素写 element(dump 字符串)与 element_json(真实 json,
///   供下游 `ctx.in<nlohmann::json>("element_json")` 结构化消费)
/// - ";" 分隔的字符串
///
/// 注意:array 端口为无类型端口——类型化端口会把绑定的黑板键钉成
/// vector<string>,json 数组将无法注入(ReactiveGuard 同款教训)。

#include <behaviortree_cpp/control_node.h>
#include <behaviortree_cpp/contrib/json.hpp>
#include <string>
#include <vector>

namespace nebulabt {

class ForEachNode : public BT::ControlNode {
public:
  ForEachNode(const std::string& name, const BT::NodeConfig& config);

  static BT::PortsList providedPorts();

  BT::NodeStatus tick() override;
  void halt() override;

private:
  /// 解析 array 端口(vector<string> / json 数组 / ';' 字面量)
  std::vector<std::string> resolveArray();

  std::vector<std::string> items_;
  nlohmann::json json_array_ = nullptr;  // json 数组来源时保存;字符串来源为 null
  nlohmann::json current_json_ = nullptr;  // 当前元素(json 来源时非 null)
  size_t index_ = 0;
};

} // namespace nebulabt
