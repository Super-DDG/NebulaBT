#include "nebulabt/core/journal.hpp"

namespace nebulabt {

void to_json(nlohmann::json& j, const JournalEvent& e) {
  j = nlohmann::json{
    {"timestamp", e.timestamp},
    {"task_id", e.task_id},
    {"task_type", e.task_type},
    {"event", e.event},
    {"data", e.data},
  };
}

void from_json(const nlohmann::json& j, JournalEvent& e) {
  e.timestamp = j.value("timestamp", "");
  e.task_id = j.value("task_id", "");
  e.task_type = j.value("task_type", "");
  e.event = j.value("event", "");
  e.data = j.value("data", nlohmann::json::object());
}

} // namespace nebulabt
