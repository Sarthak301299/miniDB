#pragma once
#include <atomic>
#include <string>

#include "page.h"

namespace minidb {
class DiskManager {
 private:
  int fd = -1;
  std::string file_name;
  std::atomic<PageId> next_page_id{0};

 public:
  explicit DiskManager(const std::string& db_file);
  ~DiskManager();
  void ReadPage(PageId page_id, char* out);
  void WritePage(PageId page_id, const char* data);
  PageId AllocatePage();
  void Sync();
};
}  // namespace minidb