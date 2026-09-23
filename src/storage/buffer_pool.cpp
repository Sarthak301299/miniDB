#include "buffer_pool.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace minidb {
Frame* BufferPool::FindFrame(PageId page_id) {
  if (!page_table.contains(page_id)) return nullptr;
  return &frames[page_table[page_id]];
}

Frame* BufferPool::EvictFrame() {
  for (auto& frame : frames) {
    if (frame.page_id == INVALID_PAGE_ID) return &frame;
  }
  for (auto it = lru.rbegin(); it != lru.rend(); ++it) {
    size_t idx = *it;
    Frame& f = frames[idx];
    if (f.pin_count == 0) {
      if (f.is_dirty) {
        wal->Flush(f.page.GetLSN());
        disk->WritePage(f.page_id, f.page.GetData());
      }
      page_table.erase(f.page_id);
      lru.erase(std::next(it).base());
      f.page_id = INVALID_PAGE_ID;
      f.is_dirty = false;
      return &f;
    }
  }
  throw std::runtime_error("BufferPool: Pool exhausted. All frames pinned");
}

BufferPool::BufferPool(size_t pool_size, DiskManager* disk, WALManager* wal)
    : pool_size(pool_size), disk(disk), wal(wal), frames(pool_size) {}

Page* BufferPool::FetchPage(PageId page_id) {
  std::lock_guard<std::mutex> lock(latch);
  if (Frame* f = FindFrame(page_id)) {
    size_t idx = static_cast<size_t>(f - &frames[0]);
    f->pin_count++;
    lru.remove(idx);
    lru.push_front(idx);
    return &(f->page);
  }
  Frame* f = EvictFrame();
  disk->ReadPage(page_id, f->page.GetData());
  f->page_id = page_id;
  f->pin_count = 1;
  f->is_dirty = false;
  size_t idx = static_cast<size_t>(f - &frames[0]);
  page_table[page_id] = idx;
  lru.push_front(idx);
  return &(f->page);
}

Page* BufferPool::NewPage(PageId* out_page_id) {
  PageId page_id = disk->AllocatePage();
  std::lock_guard<std::mutex> lock(latch);
  Frame* f = EvictFrame();
  f->page = Page{};
  f->page_id = page_id;
  f->pin_count = 1;
  f->is_dirty = true;
  size_t idx = static_cast<size_t>(f - &frames[0]);
  page_table[page_id] = idx;
  lru.push_front(idx);
  *out_page_id = page_id;
  return &(f->page);
}

void BufferPool::UnpinPage(PageId page_id, bool is_dirty) {
  std::lock_guard<std::mutex> lock(latch);
  if (Frame* f = FindFrame(page_id)) {
    if (f->pin_count > 0) f->pin_count--;
    f->is_dirty = f->is_dirty || is_dirty;
  }
}

void BufferPool::FlushPage(PageId page_id) {
  std::lock_guard<std::mutex> lock(latch);
  if (Frame* f = FindFrame(page_id)) {
    if (f->is_dirty) {
      wal->Flush(f->page.GetLSN());
      disk->WritePage(page_id, f->page.GetData());
      f->is_dirty = false;
    }
  }
}

LSN BufferPool::Checkpoint() {
  std::lock_guard<std::mutex> lock(latch);
  LSN max_lsn = INVALID_LSN;
  for (auto& [page_id, idx] : page_table) {
    Frame& f = frames[idx];
    if (f.is_dirty) {
      max_lsn = std::max(max_lsn, f.page.GetLSN());
    }
  }
  if (max_lsn != INVALID_LSN) wal->Flush(max_lsn);
  for (auto& [page_id, idx] : page_table) {
    Frame& f = frames[idx];
    if (f.is_dirty) {
      disk->WritePage(page_id, f.page.GetData());
      f.is_dirty = false;
    }
  }
  disk->Sync();
  WALRecord ckpt;
  ckpt.type = WALRecordType::CHECKPOINT;
  LSN lsn = wal->Append(ckpt);
  wal->Flush(lsn);
  return lsn;
}
}  // namespace minidb