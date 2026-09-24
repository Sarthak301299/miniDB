#include "disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace minidb {
DiskManager::DiskManager(const std::string& db_file) : file_name(db_file) {
  fd = ::open(db_file.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    throw std::runtime_error("DiskManager: failed to open db file " + db_file +
                             " : " + std::string(std::strerror(errno)));
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    throw std::runtime_error("DiskManager: fstat failed : " +
                             std::string(std::strerror(errno)));
  }
  next_page_id = st.st_size / static_cast<off_t>(PAGE_SIZE);
}

DiskManager::~DiskManager() {
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
}

void DiskManager::ReadPage(PageId page_id, char* out) {
  off_t offset = static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
  ssize_t n = ::pread(fd, out, PAGE_SIZE, offset);
  if (n < 0) {
    throw std::runtime_error("DiskManager: pread failed : " +
                             std::string(std::strerror(errno)));
  }
  if (static_cast<size_t>(n) < PAGE_SIZE) {
    std::memset(out + n, 0, PAGE_SIZE - static_cast<size_t>(n));
  }
}

void DiskManager::WritePage(PageId page_id, const char* data) {
  off_t offset = static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
  ssize_t n = ::pwrite(fd, data, PAGE_SIZE, offset);
  if (n < 0 || static_cast<size_t>(n) != PAGE_SIZE) {
    throw std::runtime_error("DiskManager: pwrite failed : " +
                             std::string(std::strerror(errno)));
  }
}

PageId DiskManager::AllocatePage() {
  return next_page_id.fetch_add(1, std::memory_order_relaxed);
}

void DiskManager::AdvancePastPage(PageId page_id) {
  PageId desired = page_id + 1;
  PageId current = next_page_id.load(std::memory_order_relaxed);
  while (current < desired &&
         !next_page_id.compare_exchange_weak(current, desired,
                                             std::memory_order_relaxed)) {
  }
}

void DiskManager::Sync() {
  if (::fsync(fd) != 0) {
    throw std::runtime_error("DiskManager: fsync failed : " +
                             std::string(std::strerror(errno)));
  }
}
}  // namespace minidb