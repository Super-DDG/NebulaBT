#pragma once
/// 结构化失败:任务终态 reason 与 explain() 的数据源。
/// 设计约定:失败是数据,不是日志行——可序列化、可查询、可回放。

#include <behaviortree_cpp/contrib/json.hpp>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace nebulabt {

/// 一条结构化失败记录
struct FailureRecord {
  std::string code;        // 机器可读错误码,如 "NAV_TIMEOUT"
  std::string node;        // 节点实例名
  std::string node_type;   // 节点注册名(如 "NavToPoint")
  std::string message;     // 人可读说明
  int attempt = 1;         // 第几次尝试(重试场景由策略层填充)
  std::string timestamp;   // "YYYY-MM-DD HH:MM:SS.mmm"
};

void to_json(nlohmann::json& j, const FailureRecord& r);
void from_json(const nlohmann::json& j, FailureRecord& r);

/// 任务级失败日志(线程安全)
class FailureLog {
public:
  void add(FailureRecord record);
  std::vector<FailureRecord> snapshot() const;
  bool empty() const;

private:
  mutable std::mutex mu_;
  std::deque<FailureRecord> records_;
};

/// 本地时间戳 "YYYY-MM-DD HH:MM:SS.mmm"
std::string nowTimestamp();

} // namespace nebulabt
