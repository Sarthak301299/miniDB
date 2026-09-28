#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "page.h"

namespace minidb {
enum class WALRecordType : uint8_t {
  BEGIN = 1,
  COMMIT,
  UPDATE,
  CHECKPOINT,
  ABORT,
};

struct WALRecord {
  LSN lsn = INVALID_LSN;
  WALRecordType type = WALRecordType::UPDATE;
  uint64_t txn_id = 0;
  PageId page_id = INVALID_PAGE_ID;
  uint32_t offset = 0;
  std::vector<char> after_image;
  std::vector<char> Serialize() const;
  static WALRecord Deserialize(const char* buf, size_t len);
};

class WALManager {
 private:
  std::string file_name;
  int fd = -1;
  bool read_only;
  std::mutex buffer_latch;
  std::mutex write_latch;
  LSN next_lsn = 1;
  std::atomic<LSN> flushed_lsn{0};
  std::vector<std::vector<char>> buffer;

 public:
  explicit WALManager(const std::string& log_file, bool read_only = false);
  ~WALManager();
  LSN Append(WALRecord record);
  void Flush(LSN upto = INVALID_LSN);
  std::vector<WALRecord> ReadAll();
  std::vector<WALRecord> ReadAll(long start_offset, long* out_end_offset,
                                 std::vector<long>* out_offsets = nullptr);
  bool ReadRecordAt(long offset, WALRecord* out) const;
  LSN FlushedLSN() const;
  bool IsReadOnly() const { return read_only; }
};
}  // namespace minidb