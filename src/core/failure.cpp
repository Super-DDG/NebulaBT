#include "nebulabt/core/failure.hpp"

#include <chrono>
#include <ctime>

namespace nebulabt {

void to_json(nlohmann::json& j, const FailureRecord& r) {
  j = nlohmann::json{
    {"code", r.code},
    {"node", r.node},
    {"node_type", r.node_type},
    {"message", r.message},
    {"attempt", r.attempt},
    {"timestamp", r.timestamp},
  };
}

void from_json(const nlohmann::json& j, FailureRecord& r) {
  r.code = j.value("code", "");
  r.node = j.value("node", "");
  r.node_type = j.value("node_type", "");
  r.message = j.value("message", "");
  r.attempt = j.value("attempt", 1);
  r.timestamp = j.value("timestamp", "");
}

void FailureLog::add(FailureRecord record) {
  std::lock_guard<std::mutex> lk(mu_);
  records_.push_back(std::move(record));
}

std::vector<FailureRecord> FailureLog::snapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  return {records_.begin(), records_.end()};
}

bool FailureLog::empty() const {
  std::lock_guard<std::mutex> lk(mu_);
  return records_.empty();
}

std::string nowTimestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  const std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>(ms.count()));
  return buf;
}

} // namespace nebulabt
