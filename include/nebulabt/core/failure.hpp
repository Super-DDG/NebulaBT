#pragma once
/// 结构化失败:任务终态 reason 与 explain() 的数据源。
/// 设计约定:失败是数据,不是日志行——可序列化、可查询、可回放。

#include <behaviortree_cpp/contrib/json.hpp>
#include <deque>
#include <map>
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
  /// 失败时的输入端口快照(激活时刻,{key} 已解析为黑板值)——
  /// 排障时不用猜"当时参数是什么"(M7 富化;桥自动填充)
  nlohmann::json inputs = nlohmann::json::object();
  /// 失败码未在登记表声明(M8):true = 拼写错误或漏声明,explain 可筛
  bool undeclared_code = false;
};

/// 失败码登记表(M8):失败码是接口——先声明后使用。
/// 表为空 = 未启用校验(known 恒 true,向后兼容)。
/// 声明入口:Engine::declareFailureCodes / pack 清单 task.failure_codes。
class FailureCodeTable {
public:
  void declare(const std::string& code, const std::string& description);
  /// 表非空且未登记 => false
  bool known(const std::string& code) const;
  std::map<std::string, std::string> snapshot() const;

private:
  mutable std::mutex mu_;
  std::map<std::string, std::string> codes_;  // code -> description
};

void to_json(nlohmann::json& j, const FailureRecord& r);
void from_json(const nlohmann::json& j, FailureRecord& r);

/// 任务级失败日志(线程安全)
class FailureLog {
public:
  void add(FailureRecord record);
  std::vector<FailureRecord> snapshot() const;
  bool empty() const;
  size_t size() const;

private:
  mutable std::mutex mu_;
  std::deque<FailureRecord> records_;
};

/// 本地时间戳 "YYYY-MM-DD HH:MM:SS.mmm"
std::string nowTimestamp();

} // namespace nebulabt
