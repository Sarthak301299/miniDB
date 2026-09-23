#include "buffer_pool.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace minidb {
size_t BufferPool::ShardFor(PageId page_id) const {
  return static_cast<size_t>(page_id) % shards.size();
}

Frame* BufferPool::FindFrameInShard(Shard& shard, PageId page_id) {
  if (!shard.page_table.contains(page_id)) return nullptr;
  return &shard.frames[shard.page_table[page_id]];
}

Frame* BufferPool::EvictFrameInShard(Shard& shard) {
  for (auto& frame : shard.frames) {
    if (frame.page_id == INVALID_PAGE_ID) return &frame;
  }
  for (auto it = shard.lru.rbegin(); it != shard.lru.rend(); ++it) {
    size_t idx = *it;
    Frame& f = shard.frames[idx];
    if (f.pin_count == 0) {
      if (f.is_dirty) {
        wal->Flush(f.page.GetLSN());
        disk->WritePage(f.page_id, f.page.GetData());
      }
      shard.page_table.erase(f.page_id);
      shard.lru.erase(std::next(it).base());
      f.page_id = INVALID_PAGE_ID;
      f.is_dirty = false;
      return &f;
    }
  }
  throw std::runtime_error("BufferPool: Shard exhausted. All frames pinned");
}

BufferPool::BufferPool(size_t pool_size, DiskManager* disk, WALManager* wal,
                       size_t num_shards)
    : pool_size(pool_size), disk(disk), wal(wal) {
  num_shards = std::max<size_t>(1, std::min(num_shards, pool_size));
  size_t base = pool_size / num_shards;
  size_t rem = pool_size % num_shards;

  shards.reserve(num_shards);
  for (size_t i = 0; i < num_shards; ++i) {
    size_t shard_frame_count = base + (i < rem ? 1 : 0);
    auto shard = std::make_unique<Shard>();
    shard->frames.resize(shard_frame_count);
    shard->lru_pos.resize(shard_frame_count);
    shards.push_back(std::move(shard));
  }
}

Page* BufferPool::FetchPage(PageId page_id) {
  Shard& shard = *shards[ShardFor(page_id)];
  std::lock_guard<std::mutex> lock(shard.latch);
  if (Frame* f = FindFrameInShard(shard, page_id)) {
    size_t idx = static_cast<size_t>(f - &shard.frames[0]);
    f->pin_count++;
    shard.lru.erase(shard.lru_pos[idx]);
    shard.lru_pos[idx] = shard.lru.insert(shard.lru.begin(), idx);
    return &(f->page);
  }
  Frame* f = EvictFrameInShard(shard);
  disk->ReadPage(page_id, f->page.GetData());
  f->page_id = page_id;
  f->pin_count = 1;
  f->is_dirty = false;
  size_t idx = static_cast<size_t>(f - &shard.frames[0]);
  shard.page_table[page_id] = idx;
  shard.lru_pos[idx] = shard.lru.insert(shard.lru.begin(), idx);
  return &(f->page);
}

Page* BufferPool::NewPage(PageId* out_page_id) {
  PageId page_id = disk->AllocatePage();
  Shard& shard = *shards[ShardFor(page_id)];
  std::lock_guard<std::mutex> lock(shard.latch);
  Frame* f = EvictFrameInShard(shard);
  f->page = Page{};
  f->page_id = page_id;
  f->pin_count = 1;
  f->is_dirty = true;
  size_t idx = static_cast<size_t>(f - &shard.frames[0]);
  shard.page_table[page_id] = idx;
  shard.lru_pos[idx] = shard.lru.insert(shard.lru.begin(), idx);
  *out_page_id = page_id;
  return &(f->page);
}

void BufferPool::UnpinPage(PageId page_id, bool is_dirty) {
  Shard& shard = *shards[ShardFor(page_id)];
  std::lock_guard<std::mutex> lock(shard.latch);
  if (Frame* f = FindFrameInShard(shard, page_id)) {
    if (!f) return;
    if (f->pin_count > 0) f->pin_count--;
    f->is_dirty = f->is_dirty || is_dirty;
  }
}

void BufferPool::FlushPage(PageId page_id) {
  Shard& shard = *shards[ShardFor(page_id)];
  std::lock_guard<std::mutex> lock(shard.latch);
  if (Frame* f = FindFrameInShard(shard, page_id)) {
    if (f && f->is_dirty) {
      wal->Flush(f->page.GetLSN());
      disk->WritePage(page_id, f->page.GetData());
      f->is_dirty = false;
    }
  }
}

LSN BufferPool::Checkpoint() {
  LSN max_lsn = INVALID_LSN;
  for (auto& shard : shards) {
    std::lock_guard<std::mutex> lock(shard->latch);
    for (auto& [page_id, idx] : shard->page_table) {
      Frame& f = shard->frames[idx];
      if (f.is_dirty && f.pin_count == 0) {
        max_lsn = std::max(max_lsn, f.page.GetLSN());
      }
    }
  }
  if (max_lsn != INVALID_LSN) wal->Flush(max_lsn);
  for (auto& shard : shards) {
    std::lock_guard<std::mutex> lock(shard->latch);
    for (auto& [page_id, idx] : shard->page_table) {
      Frame& f = shard->frames[idx];
      if (f.is_dirty && f.pin_count == 0) {
        wal->Flush(f.page.GetLSN());
        disk->WritePage(page_id, f.page.GetData());
        f.is_dirty = false;
      }
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