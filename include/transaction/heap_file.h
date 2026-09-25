#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "buffer_pool.h"
#include "page.h"
#include "transaction.h"
#include "transaction_manager.h"
#include "tuple.h"
#include "wal_manager.h"

namespace minidb {
struct RID {
  PageId page_id = INVALID_PAGE_ID;
  uint32_t slot = 0;
  bool operator==(const RID& other) const {
    return page_id == other.page_id && slot == other.slot;
  }
};

struct SlotPageHeader {
  uint16_t num_slots = 0;
  uint16_t free_start = 0;
  uint16_t free_end = 0;
};

struct Slot {
  uint16_t offset = 0;
  uint16_t length = 0;
};

class HeapFile {
 private:
  void InitPage(Page* page);
  PageId AllocateNewPage();
  static constexpr size_t num_shards = 16;
  std::mutex& GetShardLatch(PageId page_id) const;
  mutable std::array<std::mutex, num_shards> shard_latches;
  BufferPool* pool;
  WALManager* wal;
  mutable std::mutex latch;
  std::vector<PageId> pages;
  PageId current_insert_page = INVALID_PAGE_ID;
  std::function<void(PageId)> on_new_page;

 public:
  HeapFile(BufferPool* pool, WALManager* wal);
  HeapFile(BufferPool* pool, WALManager* wal,
           std::vector<PageId> existing_pages);
  void SetOnNewPageCallback(std::function<void(PageId)> cb) {
    on_new_page = cb;
  };
  RID InsertTuple(Transaction* txn, const std::vector<char>& data);
  bool DeleteTuple(Transaction* txn, const RID rid);
  std::vector<std::pair<RID, std::vector<char>>> Scan(
      TxnId txn_id, const TransactionManager& manager);
  bool ReadTuple(RID rid, TupleHeader* out_header, std::vector<char>* out_data);
  std::vector<PageId> GetAllPageIds() const;
};
}  // namespace minidb