#pragma once
#include <list>
#include <unordered_map>
#include <vector>

#include "disk_manager.h"
#include "page.h"
#include "wal_manager.h"

namespace minidb {
struct Frame {
  Page page;
  PageId page_id = INVALID_PAGE_ID;
  int pin_count = 0;
  bool is_dirty = false;
};

class BufferPool {
 private:
  Frame* FindFrame(PageId page_id);
  Frame* EvictFrame();
  size_t pool_size;
  DiskManager* disk;
  WALManager* wal;
  std::vector<Frame> frames;
  std::unordered_map<PageId, size_t> page_table;
  std::list<size_t> lru;
  std::mutex latch;

 public:
  BufferPool(size_t pool_size, DiskManager* disk, WALManager* wal);
  Page* FetchPage(PageId page_id);
  Page* NewPage(PageId* out_page_id);
  void UnpinPage(PageId page_id, bool is_dirty);
  void FlushPage(PageId page_id);
  LSN Checkpoint();
};
}  // namespace minidb