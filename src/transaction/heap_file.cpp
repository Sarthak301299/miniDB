#include "heap_file.h"

namespace minidb {
void HeapFile::InitPage(Page* page) {
  SlotPageHeader header;
  header.num_slots = 0;
  header.free_start = sizeof(SlotPageHeader);
  header.free_end = Page::UsableSize();
  std::memcpy(page->GetData() + Page::HeaderSize(), &header, sizeof(header));
}

PageId HeapFile::AllocateNewPage() {
  PageId page_id;
  Page* page = pool->NewPage(&page_id);
  InitPage(page);
  pool->UnpinPage(page_id, true);
  pages.push_back(page_id);
  return page_id;
}

std::mutex& HeapFile::GetShardLatch(PageId page_id) const {
  return shard_latches[static_cast<size_t>(page_id) % num_shards];
}

HeapFile::HeapFile(BufferPool* pool, WALManager* wal) : pool(pool), wal(wal) {}

RID HeapFile::InsertTuple(Transaction* txn, const std::vector<char>& data) {
  TupleHeader header;
  header.xmin = txn->id;
  header.xmax = INVALID_TXN_ID;
  header.data_len = static_cast<uint32_t>(data.size());
  size_t needed = sizeof(Slot) + sizeof(TupleHeader) + data.size();
  while (true) {
    PageId target;
    {
      std::lock_guard<std::mutex> lock(latch);
      if (current_insert_page == INVALID_PAGE_ID) {
        current_insert_page = AllocateNewPage();
      }
      target = current_insert_page;
    }
    std::lock_guard<std::mutex> shard_lock(GetShardLatch(target));
    Page* page = pool->FetchPage(target);
    char* base = page->GetData() + Page::HeaderSize();
    SlotPageHeader page_header;
    std::memcpy(&page_header, base, sizeof(page_header));
    if (static_cast<size_t>(page_header.free_end - page_header.free_start) <
        needed) {
      pool->UnpinPage(target, false);
      {
        std::lock_guard<std::mutex> lock(latch);
        if (current_insert_page == target)
          current_insert_page = AllocateNewPage();
      }
      continue;
    }
    uint16_t tuple_length =
        static_cast<uint16_t>(sizeof(TupleHeader) + data.size());
    uint16_t tuple_offset = page_header.free_end - tuple_length;
    std::memcpy(base + tuple_offset, &header, sizeof(header));
    if (!data.empty()) {
      std::memcpy(base + tuple_offset + sizeof(header), data.data(),
                  data.size());
    }
    Slot slot;
    slot.offset = tuple_offset;
    slot.length = tuple_length;
    uint16_t slot_ptr = page_header.free_start;
    std::memcpy(base + slot_ptr, &slot, sizeof(slot));
    uint16_t new_slot_index = page_header.num_slots;
    page_header.num_slots++;
    page_header.free_start += sizeof(Slot);
    page_header.free_end = tuple_offset;
    std::memcpy(base, &page_header, sizeof(page_header));

    WALRecord record;
    record.type = WALRecordType::UPDATE;
    record.page_id = target;
    record.txn_id = txn->id;
    record.offset = 0;
    record.after_image.assign(base, base + Page::UsableSize());
    LSN lsn = wal->Append(record);
    page->SetLSN(lsn);
    pool->UnpinPage(target, true);
    return RID{target, new_slot_index};
  }
}
bool HeapFile::DeleteTuple(Transaction* txn, const RID rid) {
  std::lock_guard<std::mutex> shard_lock(GetShardLatch(rid.page_id));
  Page* page = pool->FetchPage(rid.page_id);
  char* base = page->GetData() + Page::HeaderSize();
  SlotPageHeader page_header;
  std::memcpy(&page_header, base, sizeof(page_header));
  if (rid.slot >= page_header.num_slots) {
    pool->UnpinPage(rid.page_id, false);
    return false;
  }
  Slot slot;
  std::memcpy(&slot, base + sizeof(SlotPageHeader) + rid.slot * sizeof(Slot),
              sizeof(slot));
  TupleHeader header;
  std::memcpy(&header, base + slot.offset, sizeof(header));
  header.xmax = txn->id;
  std::memcpy(base + slot.offset, &header, sizeof(header));

  WALRecord record;
  record.type = WALRecordType::UPDATE;
  record.page_id = rid.page_id;
  record.txn_id = txn->id;
  record.offset = 0;
  record.after_image.assign(base, base + Page::UsableSize());
  LSN lsn = wal->Append(record);
  page->SetLSN(lsn);
  pool->UnpinPage(rid.page_id, true);
  return true;
}

std::vector<std::pair<RID, std::vector<char>>> HeapFile::Scan(
    TxnId txn_id, const TransactionManager& manager) {
  std::vector<std::pair<RID, std::vector<char>>> out;
  std::vector<PageId> pages_copy;
  {
    std::lock_guard<std::mutex> lock(latch);
    pages_copy = pages;
  }
  std::array<std::vector<PageId>, num_shards> pages_by_shards;
  for (PageId page_id : pages_copy) {
    pages_by_shards[static_cast<size_t>(page_id) % num_shards].push_back(
        page_id);
  }
  for (size_t shard = 0; shard < num_shards; ++shard) {
    if (!pages_by_shards[shard].empty()) {
      std::lock_guard<std::mutex> shard_lock(shard_latches[shard]);
      for (PageId page_id : pages_by_shards[shard]) {
        Page* page = pool->FetchPage(page_id);
        char* base = page->GetData() + Page::HeaderSize();
        SlotPageHeader page_header;
        std::memcpy(&page_header, base, sizeof(page_header));
        for (uint16_t slot_index = 0; slot_index < page_header.num_slots;
             ++slot_index) {
          Slot slot;
          std::memcpy(&slot,
                      base + sizeof(SlotPageHeader) + slot_index * sizeof(Slot),
                      sizeof(slot));
          TupleHeader header;
          std::memcpy(&header, base + slot.offset, sizeof(header));
          if (manager.isVisible(header, txn_id)) {
            std::vector<char> data(slot.length - sizeof(TupleHeader));
            if (header.data_len > 0) {
              std::memcpy(data.data(), base + slot.offset + sizeof(TupleHeader),
                          header.data_len);
            }
            out.push_back({RID{page_id, slot_index}, std::move(data)});
          }
        }
        pool->UnpinPage(page_id, false);
      }
    }
  }
  return out;
}

bool HeapFile::ReadTuple(RID rid, TupleHeader* out_header,
                         std::vector<char>* out_data) {
  std::lock_guard<std::mutex> shard_lock(GetShardLatch(rid.page_id));
  Page* page = pool->FetchPage(rid.page_id);
  char* base = page->GetData() + Page::HeaderSize();
  SlotPageHeader page_header;
  std::memcpy(&page_header, base, sizeof(page_header));
  if (rid.slot >= page_header.num_slots) {
    pool->UnpinPage(rid.page_id, false);
    return false;
  }
  Slot slot;
  std::memcpy(&slot, base + sizeof(SlotPageHeader) + rid.slot * sizeof(Slot),
              sizeof(slot));
  std::memcpy(out_header, base + slot.offset, sizeof(TupleHeader));
  if (out_header->data_len > 0) {
    out_data->resize(out_header->data_len);
    std::memcpy(out_data->data(), base + slot.offset + sizeof(TupleHeader),
                out_header->data_len);
  }
  pool->UnpinPage(rid.page_id, false);
  return true;
}
std::vector<PageId> HeapFile::GetAllPageIds() const {
  std::lock_guard<std::mutex> lock(latch);
  return pages;
}
}  // namespace minidb