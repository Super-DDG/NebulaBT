#include "nebulabt/core/journal_file.hpp"

#include <cstdint>
#include <cstring>
#include <array>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace nebulabt {

namespace {

// CRC-32(IEEE 802.3,poly 0xEDB88320),查表实现,无外部依赖。
// 表在编译期生成:写侧(sink 回调)与读侧(恢复线程)可能首次并发
// 调用,运行期惰性初始化存在数据竞争
constexpr std::array<uint32_t, 256> makeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}
constexpr auto kCrcTable = makeCrcTable();

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    c = kCrcTable[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

void putU32(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

// ============================================================================
// JournalFileWriter
// ============================================================================

JournalFileWriter::~JournalFileWriter() {
  close();
}

bool JournalFileWriter::open(const std::string& path, bool append,
                             JournalFsync fsync, std::string* error) {
  close();
  std::FILE* f = std::fopen(path.c_str(), append ? "ab" : "wb");
  if (!f) {
    if (error) {
      *error = "cannot open " + path;
    }
    return false;
  }
  f_ = f;
  fsync_ = fsync;
  return true;
}

bool JournalFileWriter::append(const JournalEvent& e, std::string* error) {
  if (!f_) {
    if (error) {
      *error = "journal file not open";
    }
    return false;
  }
  const std::string payload = nlohmann::json(e).dump();
  std::vector<uint8_t> frame;
  frame.reserve(8 + payload.size());
  putU32(frame, static_cast<uint32_t>(payload.size()));
  putU32(frame, crc32(reinterpret_cast<const uint8_t*>(payload.data()),
                      payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());

  if (std::fwrite(frame.data(), 1, frame.size(), f_) != frame.size()) {
    if (error) {
      *error = "journal write failed";
    }
    return false;
  }
  if (std::fflush(f_) != 0) {
    if (error) {
      *error = "journal flush failed";
    }
    return false;
  }
#if !defined(_WIN32)
  if (fsync_ == JournalFsync::PerEvent) {
    if (::fsync(::fileno(f_)) != 0) {
      if (error) {
        *error = "journal fsync failed";
      }
      return false;
    }
  }
#endif
  return true;
}

void JournalFileWriter::close() {
  if (f_) {
    std::fclose(f_);
    f_ = nullptr;
  }
}

// ============================================================================
// readJournalFile
// ============================================================================

bool readJournalFile(const std::string& path, nlohmann::json& out_events,
                     JournalFileReadStats* stats, std::string* error) {
  JournalFileReadStats local;
  out_events = nlohmann::json::array();

  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (error) {
      *error = "cannot open " + path;
    }
    return false;
  }
  std::vector<uint8_t> data;
  {
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      data.insert(data.end(), buf, buf + n);
    }
    std::fclose(f);
  }

  size_t off = 0;
  while (true) {
    const size_t remaining = data.size() - off;
    if (remaining == 0) {
      break;  // 完整读完
    }
    if (remaining < 8) {
      local.torn_tail = true;  // 头都不完整:掉电窗口
      break;
    }
    const uint32_t len = getU32(&data[off]);
    const uint32_t crc = getU32(&data[off + 4]);
    // len 上限保护:防止损坏头导致超大分配(> 64MB 视为损坏)
    if (len == 0 || len > (64u << 20) ||
        remaining - 8 < len) {
      local.torn_tail = true;
      break;
    }
    const uint8_t* payload = &data[off + 8];
    if (crc32(payload, len) != crc) {
      local.torn_tail = true;  // CRC 不符:帧被写坏
      break;
    }
    try {
      out_events.push_back(
          nlohmann::json::parse(reinterpret_cast<const char*>(payload),
                                reinterpret_cast<const char*>(payload) + len));
    } catch (const std::exception&) {
      local.torn_tail = true;
      break;
    }
    local.good_records++;
    off += 8 + len;
  }

  if (stats) {
    *stats = local;
  }
  return true;
}

} // namespace nebulabt
