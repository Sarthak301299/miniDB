#include "wal_manager.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace minidb {
namespace {
LSN PeekLSN(const std::vector<char>& bytes) {
  LSN lsn;
  std::memcpy(&lsn, bytes.data(), sizeof(lsn));
  return lsn;
}
}  // namespace

std::vector<char> WALRecord::Serialize() const {
  std::vector<char> buffer;
  auto append = [&](const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    buffer.insert(buffer.end(), c, c + n);
  };
  append(&lsn, sizeof(lsn));
  append(&type, sizeof(type));
  append(&txn_id, sizeof(txn_id));
  append(&page_id, sizeof(page_id));
  append(&offset, sizeof(offset));
  uint32_t len = static_cast<uint32_t>(after_image.size());
  append(&len, sizeof(len));
  if (len > 0) append(after_image.data(), len);
  return buffer;
}

WALRecord WALRecord::Deserialize(const char* buf, size_t len) {
  constexpr size_t fixed_size = sizeof(lsn) + sizeof(WALRecordType) +
                                sizeof(uint64_t) + sizeof(PageId) +
                                sizeof(uint32_t) + sizeof(uint32_t);
  if (len < fixed_size) {
    throw std::runtime_error(
        "WAL Record::Deserialize: buffer too small for fixed header "
        "(truncated/corrupt record)");
  }

  WALRecord r;
  size_t off = 0;
  auto read = [&](void* p, size_t n) {
    std::memcpy(p, buf + off, n);
    off += n;
  };
  read(&r.lsn, sizeof(r.lsn));
  read(&r.type, sizeof(r.type));
  read(&r.txn_id, sizeof(r.txn_id));
  read(&r.page_id, sizeof(r.page_id));
  read(&r.offset, sizeof(r.offset));
  uint32_t alen = 0;
  read(&alen, sizeof(alen));
  if (alen > len - off) {
    throw std::runtime_error(
        "WAL Record::Deserialize: after_image length exceeds buffer bounds "
        "(corrupt/torn record)");
  }
  r.after_image.resize(alen);
  if (alen > 0) {
    read(r.after_image.data(), alen);
  }
  return r;
}

WALManager::WALManager(const std::string& log_file) : file_name(log_file) {
  fd = ::open(log_file.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    throw std::runtime_error("WALManager: open failed for " + log_file + " : " +
                             std::string(std::strerror(errno)));
  }
  auto existing = ReadAll();
  if (!existing.empty()) {
    flushed_lsn.store(existing.back().lsn);
    next_lsn = flushed_lsn.load() + 1;
  }
}

WALManager::~WALManager() {
  Flush();
  if (fd >= 0) ::close(fd);
}

LSN WALManager::Append(WALRecord record) {
  std::lock_guard<std::mutex> lock(buffer_latch);
  record.lsn = next_lsn++;
  buffer.push_back(record.Serialize());
  return record.lsn;
}

void WALManager::Flush(LSN upto) {
  std::lock_guard<std::mutex> write_lock(write_latch);
  std::vector<std::vector<char>> to_write;
  {
    std::lock_guard<std::mutex> buffer_lock(buffer_latch);
    if (buffer.empty()) return;
    if (upto == INVALID_LSN) {
      to_write.swap(buffer);
    } else {
      auto it = buffer.begin();
      while (it != buffer.end() && PeekLSN(*it) <= upto) ++it;
      to_write.assign(std::make_move_iterator(buffer.begin()),
                      std::make_move_iterator(it));
      buffer.erase(buffer.begin(), it);
    }
  }
  if (to_write.empty()) return;
  LSN last_written = flushed_lsn.load();
  for (auto& bytes : to_write) {
    LSN lsn = PeekLSN(bytes);
    uint32_t len = static_cast<uint32_t>(bytes.size());
    if (::write(fd, &len, sizeof(len)) != static_cast<ssize_t>(sizeof(len)) ||
        ::write(fd, bytes.data(), bytes.size()) !=
            static_cast<ssize_t>(sizeof(bytes.size()))) {
      throw std::runtime_error("WALManager: write failed: " +
                               std::string(std::strerror(errno)));
    }
    last_written = lsn;
  }
  if (::fsync(fd) != 0) {
    throw std::runtime_error("WALManager: fsync failed: " +
                             std::string(std::strerror(errno)));
  }
  flushed_lsn.store(last_written);
}

std::vector<WALRecord> WALManager::ReadAll() {
  std::vector<WALRecord> records;
  int rfd = ::open(file_name.c_str(), O_RDONLY);
  if (rfd < 0) return records;
  while (true) {
    uint32_t len = 0;
    ssize_t n = ::read(rfd, &len, sizeof(len));
    if (n != static_cast<ssize_t>(sizeof(len))) break;
    constexpr uint32_t max_record_size = 16 * 1024 * 1024;
    if (len > max_record_size) break;
    std::vector<char> buf(len);
    n = ::read(rfd, buf.data(), len);
    if (n != static_cast<ssize_t>(len)) break;
    try {
      records.push_back(WALRecord::Deserialize(buf.data(), len));
    } catch (const std::exception&) {
      break;
    }
  }
  ::close(rfd);
  return records;
}

LSN WALManager::FlushedLSN() const { return flushed_lsn; }
}  // namespace minidb