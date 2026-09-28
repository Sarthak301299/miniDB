#pragma once
#include <functional>
#include <list>
#include <memory>
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
  bool stale = false;
};

struct Shard {
  std::mutex latch;
  std::vector<Frame> frames;
  std::unordered_map<PageId, size_t> page_table;
  std::list<size_t> lru;
  std::vector<std::list<size_t>::iterator> lru_pos;
};

class BufferPool {
 private:
  size_t ShardFor(PageId page_id) const;
  Frame* FindFrameInShard(Shard& shard, PageId page_id);
  Frame* EvictFrameInShard(Shard& shard);
  size_t pool_size;
  DiskManager* disk;
  WALManager* wal;
  std::vector<std::unique_ptr<Shard>> shards;
  std::function<void(PageId)> pre_disk_read_hook;
  std::function<void(PageId, LSN)> post_disk_read_hook;

 public:
  BufferPool(size_t pool_size, DiskManager* disk, WALManager* wal,
             size_t num_shards = 8);
  Page* FetchPage(PageId page_id);
  Page* NewPage(PageId* out_page_id);
  void UnpinPage(PageId page_id, bool is_dirty);
  void FlushPage(PageId page_id);
  void InvalidatePage(PageId page_id);
  LSN Checkpoint();
  void SetPreDiskReadHook(std::function<void(PageId)> hook) {
    pre_disk_read_hook = std::move(hook);
  }
  void SetPostDiskReadHook(std::function<void(PageId, LSN)> hook) {
    post_disk_read_hook = std::move(hook);
  }
};
}  // namespace minidb