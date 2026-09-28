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
  bool read_only = false;

 public:
  explicit DiskManager(const std::string& db_file, bool read_only = false);
  ~DiskManager();
  void ReadPage(PageId page_id, char* out);
  void WritePage(PageId page_id, const char* data);
  PageId AllocatePage();
  bool IsReadOnly() const { return read_only; }
  void AdvancePastPage(PageId page_id);
  void Sync();
};
}  // namespace minidb