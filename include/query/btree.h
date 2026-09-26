#pragma once
#include <atomic>
#include <optional>
#include <vector>

#include "heap_file.h"

namespace minidb {
constexpr size_t btreeMaxLeafEntries = 8;
constexpr size_t btreeMaxInternalKeys = 8;

class BPlusTree {
 private:
  struct LeafEntry {
    int64_t key;
    PageId page_id;
    uint16_t slot;
  };
  struct LeafNode {
    PageId next_leaf = INVALID_PAGE_ID;
    std::vector<LeafEntry> entries;
  };
  struct InternalNode {
    std::vector<int64_t> keys;
    std::vector<PageId> children;
  };

  static bool IsLeafPage(Page* page);
  static LeafNode ReadLeafNode(Page* page);
  static InternalNode ReadInternalNode(Page* page);

  void WriteLeafNode(PageId page_id, Page* page, const LeafNode& node);
  void WriteInternalNode(PageId page_id, Page* page, const InternalNode& node);
  void PersistPage(PageId page_id, Page* page, const std::vector<char>& bytes);

  std::optional<std::pair<int64_t, PageId>> InsertRecursive(PageId node_id,
                                                            int64_t key,
                                                            RID rid);
  std::optional<std::pair<int64_t, PageId>> InsertIntoLeaf(PageId node_id,
                                                           int64_t key,
                                                           RID rid);
  std::optional<std::pair<int64_t, PageId>> InsertIntoInternal(PageId node_id,
                                                               int64_t key,
                                                               RID rid);

  BufferPool* pool;
  WALManager* wal;
  std::atomic<PageId> root_page_id;
  mutable std::mutex latch;
  std::function<void(PageId)> on_root_change;

 public:
  BPlusTree(BufferPool* pool, WALManager* wal);
  BPlusTree(BufferPool* pool, WALManager* wal, PageId root_page_id);

  PageId RootPageId() const { return root_page_id.load(); }

  void SetOnRootChangeCallback(std::function<void(PageId)> cb) {
    on_root_change = std::move(cb);
  }

  void Insert(int64_t key, RID rid);
  std::vector<RID> Search(int64_t key) const;
  std::vector<RID> RangeScan(int64_t lo, int64_t hi) const;
};
}  // namespace minidb