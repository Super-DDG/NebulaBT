#pragma once
/// Journal 文件持久化(M4):掉电安全的帧格式 + 容错读取。
///
/// 落盘格式:每条事件一个帧
///   [u32 payload_len(LE)][u32 crc32(LE)][payload(len 字节)]
/// payload = JournalEvent 的 JSON(紧凑格式)。
///
/// 掉电语义:崩溃瞬间最多损坏"正在写的最后一帧"——读取侧遇到
/// 截断尾帧或 CRC 不符即停止,保留之前的完好前缀,不做整文件报废。
/// 恢复侧对 torn_tail 应视为正常情况(掉电窗口),而非错误。
///
/// fsync 策略(嵌入式 flash 磨损 vs 数据安全的权衡):
///   JournalFsync::Never     只 fflush,最快,依赖 OS 刷盘
///   JournalFsync::PerEvent  每事件 fsync,最安全,flash 写放大最大

#include "nebulabt/core/journal.hpp"
#include <cstdio>
#include <string>
#include <vector>

namespace nebulabt {

enum class JournalFsync { Never, PerEvent };

/// 追加式 journal 文件写侧。线程安全性:同一时刻仅一个线程调用 append
/// (与 setJournalSink 的调度线程回调一致);open/close 与 append 互斥由调用方保证。
class JournalFileWriter {
public:
  JournalFileWriter() = default;
  ~JournalFileWriter();
  JournalFileWriter(const JournalFileWriter&) = delete;
  JournalFileWriter& operator=(const JournalFileWriter&) = delete;

  /// append=false 时覆盖已有文件;失败返回 false 并填写 error
  bool open(const std::string& path, bool append,
            JournalFsync fsync = JournalFsync::Never,
            std::string* error = nullptr);

  bool append(const JournalEvent& e, std::string* error = nullptr);
  void close();

private:
  std::FILE* f_ = nullptr;
  JournalFsync fsync_ = JournalFsync::Never;
};

struct JournalFileReadStats {
  size_t good_records = 0;
  bool torn_tail = false;  // 结尾存在截断/损坏的帧(掉电窗口),前缀已保留
};

/// 读取 journal 文件 → out_events(JSON 数组,可直接作为
/// TaskSpec.resume_journal)。遇到撕裂/损坏帧:保留完好前缀并标记
/// torn_tail。文件不存在/IO 错误返回 false。
bool readJournalFile(const std::string& path, nlohmann::json& out_events,
                     JournalFileReadStats* stats = nullptr,
                     std::string* error = nullptr);

} // namespace nebulabt
