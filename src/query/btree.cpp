#include "btree.h"

#include <algorithm>

namespace minidb {

namespace {
template <typename T>
concept allowed = std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
                  std::is_same_v<T, int64_t>;

template <allowed T>
void Append(std::vector<char>* buf, T v) {
  const char* p = reinterpret_cast<const char*>(&v);
  buf->insert(buf->end(), p, p + sizeof(v));
}

class Reader {
 private:
  const char* data;
  size_t len;
  size_t pos = 0;
  void Need(size_t n) {
    if (pos + n > len)
      throw std::runtime_error("BPlusTree: corrupt node page (truncated read)");
  }

 public:
  Reader(const char* data, size_t len) : data(data), len(len) {}
  template <allowed T>
  T Read() {
    Need(sizeof(T));
    T v;
    std::memcpy(&v, data + pos, sizeof(v));
    pos += sizeof(v);
    return v;
  }
};
}  // namespace

bool BPlusTree::IsLeafPage(Page* page) {
  return static_cast<uint8_t>(page->GetData()[Page::HeaderSize()]) == 1;
}

BPlusTree::LeafNode BPlusTree::ReadLeafNode(Page* page) {
  Reader r(page->GetData() + Page::HeaderSize(), Page::UsableSize());
  LeafNode node;
  r.Read<uint8_t>();
  uint16_t num_entries = r.Read<uint16_t>();
  node.next_leaf = r.Read<int64_t>();
  node.entries.reserve(num_entries);
  for (uint16_t i = 0; i < num_entries; ++i) {
    LeafEntry e;
    e.key = r.Read<int64_t>();
    e.page_id = r.Read<int64_t>();
    e.slot = r.Read<uint16_t>();
    node.entries.push_back(e);
  }
  return node;
}

BPlusTree::InternalNode BPlusTree::ReadInternalNode(Page* page) {
  Reader r(page->GetData() + Page::HeaderSize(), Page::UsableSize());
  InternalNode node;
  r.Read<uint8_t>();
  uint16_t num_keys = r.Read<uint16_t>();
  node.keys.reserve(num_keys);
  node.children.reserve(static_cast<size_t>(num_keys) + 1);
  for (uint16_t i = 0; i < num_keys; ++i)
    node.keys.push_back(r.Read<int64_t>());
  for (uint16_t i = 0; i < num_keys + 1; ++i)
    node.children.push_back(static_cast<PageId>(r.Read<int64_t>()));
  return node;
}

void BPlusTree::WriteLeafNode(PageId page_id, Page* page,
                              const LeafNode& node) {
  std::vector<char> bytes;
  Append<uint8_t>(&bytes, 1);
  Append<uint16_t>(&bytes, static_cast<uint16_t>(node.entries.size()));
  Append<int64_t>(&bytes, node.next_leaf);
  for (const auto& e : node.entries) {
    Append<int64_t>(&bytes, e.key);
    Append<int64_t>(&bytes, e.page_id);
    Append<uint16_t>(&bytes, e.slot);
  }
  PersistPage(page_id, page, bytes);
}

void BPlusTree::WriteInternalNode(PageId page_id, Page* page,
                                  const InternalNode& node) {
  std::vector<char> bytes;
  Append<uint8_t>(&bytes, 0);
  Append<uint16_t>(&bytes, static_cast<uint16_t>(node.keys.size()));
  for (const auto& key : node.keys) Append<int64_t>(&bytes, key);
  for (const auto& child : node.children) Append<int64_t>(&bytes, child);
  PersistPage(page_id, page, bytes);
}

void BPlusTree::PersistPage(PageId page_id, Page* page,
                            const std::vector<char>& bytes) {
  if (bytes.size() > Page::UsableSize()) {
    throw std::runtime_error(
        "BPlusTree: A single key's duplicate entries (" +
        std::to_string(bytes.size()) +
        " bytes serialized) exceed one page's capacity (" +
        std::to_string(Page::UsableSize()) +
        " bytes). Too many physically identical keys for this tree's "
        "single-page-per-leaf design.");
  }
  char* base = page->GetData() + Page::HeaderSize();
  std::memcpy(base, bytes.data(), bytes.size());
  std::memset(base + bytes.size(), 0, Page::UsableSize() - bytes.size());

  WALRecord record;
  record.type = WALRecordType::UPDATE;
  record.page_id = page_id;
  record.offset = 0;
  record.txn_id = INVALID_TXN_ID;
  record.after_image.assign(base, base + Page::UsableSize());
  LSN lsn = wal->Append(record);
  page->SetLSN(lsn);
}

std::optional<std::pair<int64_t, PageId>> BPlusTree::InsertRecursive(
    PageId node_id, int64_t key, RID rid) {
  Page* page = pool->FetchPage(node_id);
  bool leaf = IsLeafPage(page);
  pool->UnpinPage(node_id, false);
  if (leaf) return InsertIntoLeaf(node_id, key, rid);
  return InsertIntoInternal(node_id, key, rid);
}
std::optional<std::pair<int64_t, PageId>> BPlusTree::InsertIntoLeaf(
    PageId node_id, int64_t key, RID rid) {
  Page* page = pool->FetchPage(node_id);
  LeafNode node = ReadLeafNode(page);
  auto it =
      std::upper_bound(node.entries.begin(), node.entries.end(), key,
                       [](int64_t k, const LeafEntry& e) { return k < e.key; });
  node.entries.insert(it, LeafEntry{key, rid.page_id, rid.slot});
  if (node.entries.size() < btreeMaxLeafEntries) {
    WriteLeafNode(node_id, page, node);
    pool->UnpinPage(node_id, true);
    return std::nullopt;
  }
  if (node.entries.front().key == node.entries.back().key) {
    WriteLeafNode(node_id, page, node);
    pool->UnpinPage(node_id, true);
    return std::nullopt;
  }
  size_t mid = node.entries.size() / 2;
  int64_t mid_key = node.entries[mid].key;
  size_t run_start = mid;
  while (run_start > 0 && node.entries[run_start - 1].key == mid_key)
    run_start--;
  size_t run_end = mid;
  while (run_end < node.entries.size() && node.entries[run_end].key == mid_key)
    run_end++;
  if (run_start > 0)
    mid = run_start;
  else
    mid = run_end;
  LeafNode right;
  right.entries.assign(node.entries.begin() + static_cast<long>(mid),
                       node.entries.end());
  right.next_leaf = node.next_leaf;
  node.entries.resize(mid);

  PageId right_pid;
  Page* right_page = pool->NewPage(&right_pid);
  node.next_leaf = right_pid;

  WriteLeafNode(right_pid, right_page, right);
  pool->UnpinPage(right_pid, true);
  WriteLeafNode(node_id, page, node);
  pool->UnpinPage(node_id, true);

  return std::make_pair(right.entries.front().key, right_pid);
}
std::optional<std::pair<int64_t, PageId>> BPlusTree::InsertIntoInternal(
    PageId node_id, int64_t key, RID rid) {
  Page* page = pool->FetchPage(node_id);
  InternalNode node = ReadInternalNode(page);
  pool->UnpinPage(node_id, false);

  size_t idx = 0;
  while (idx < node.keys.size() && key >= node.keys[idx]) idx++;
  PageId child_id = node.children[idx];

  auto split = InsertRecursive(child_id, key, rid);
  if (!split) return std::nullopt;
  auto [sep_key, new_child_id] = *split;

  page = pool->FetchPage(node_id);
  node = ReadInternalNode(page);
  node.keys.insert(node.keys.begin() + static_cast<long>(idx), sep_key);
  node.children.insert(node.children.begin() + static_cast<long>(idx) + 1,
                       new_child_id);
  if (node.keys.size() <= btreeMaxInternalKeys) {
    WriteInternalNode(node_id, page, node);
    pool->UnpinPage(node_id, true);
    return std::nullopt;
  }
  size_t mid = node.keys.size() / 2;
  int64_t promoted = node.keys[mid];
  InternalNode right;
  right.keys.assign(node.keys.begin() + static_cast<long>(mid) + 1,
                    node.keys.end());
  right.children.assign(node.children.begin() + static_cast<long>(mid) + 1,
                        node.children.end());
  node.keys.resize(mid);
  node.children.resize(mid + 1);

  PageId right_pid;
  Page* right_page = pool->NewPage(&right_pid);
  WriteInternalNode(right_pid, right_page, right);
  pool->UnpinPage(right_pid, true);
  WriteInternalNode(node_id, page, node);
  pool->UnpinPage(node_id, true);

  return std::make_pair(promoted, right_pid);
}

BPlusTree::BPlusTree(BufferPool* pool, WALManager* wal)
    : pool(pool), wal(wal), root_page_id(INVALID_PAGE_ID) {
  PageId root_id;
  Page* page = pool->NewPage(&root_id);
  WriteLeafNode(root_id, page, LeafNode{});
  pool->UnpinPage(root_id, true);
  root_page_id.store(root_id);
}
BPlusTree::BPlusTree(BufferPool* pool, WALManager* wal, PageId root_page_id)
    : pool(pool), wal(wal), root_page_id(root_page_id) {}

void BPlusTree::Insert(int64_t key, RID rid) {
  bool root_changed = false;
  {
    std::lock_guard<std::mutex> lock(latch);
    auto split = InsertRecursive(root_page_id.load(), key, rid);
    if (split) {
      auto [sep_key, new_child_id] = *split;
      InternalNode new_root;
      new_root.keys = {sep_key};
      new_root.children = {root_page_id.load(), new_child_id};
      PageId new_root_id;
      Page* new_root_page = pool->NewPage(&new_root_id);
      WriteInternalNode(new_root_id, new_root_page, new_root);
      pool->UnpinPage(new_root_id, true);
      root_page_id.store(new_root_id);
      root_changed = true;
    }
  }
  if (root_changed && on_root_change) on_root_change(root_page_id.load());
}
std::vector<RID> BPlusTree::Search(int64_t key) const {
  std::lock_guard<std::mutex> lock(latch);
  PageId node_id = root_page_id.load();
  while (true) {
    Page* page = pool->FetchPage(node_id);
    if (IsLeafPage(page)) {
      pool->UnpinPage(node_id, false);
      break;
    }
    InternalNode node = ReadInternalNode(page);
    pool->UnpinPage(node_id, false);
    size_t idx = 0;
    while (idx < node.keys.size() && key >= node.keys[idx]) idx++;
    node_id = node.children[idx];
  }

  std::vector<RID> results;
  PageId cur = node_id;
  while (cur != INVALID_PAGE_ID) {
    Page* p = pool->FetchPage(cur);
    LeafNode node = ReadLeafNode(p);
    pool->UnpinPage(cur, false);
    bool last_matched = false;
    for (const auto& e : node.entries) {
      if (e.key == key) {
        results.push_back(RID{e.page_id, e.slot});
        last_matched = true;
      } else {
        last_matched = false;
      }
    }
    if (!last_matched) break;
    cur = node.next_leaf;
  }
  return results;
}
std::vector<RID> BPlusTree::RangeScan(int64_t lo, int64_t hi) const {
  std::lock_guard<std::mutex> lock(latch);
  PageId node_id = root_page_id.load();
  while (true) {
    Page* page = pool->FetchPage(node_id);
    if (IsLeafPage(page)) {
      pool->UnpinPage(node_id, false);
      break;
    }
    InternalNode node = ReadInternalNode(page);
    pool->UnpinPage(node_id, false);
    size_t idx = 0;
    while (idx < node.keys.size() && lo >= node.keys[idx]) idx++;
    node_id = node.children[idx];
  }

  std::vector<RID> results;
  PageId cur = node_id;
  while (cur != INVALID_PAGE_ID) {
    Page* p = pool->FetchPage(cur);
    LeafNode node = ReadLeafNode(p);
    pool->UnpinPage(cur, false);
    for (const auto& e : node.entries) {
      if (e.key > hi) return results;
      if (e.key >= lo) results.push_back(RID{e.page_id, e.slot});
    }
    cur = node.next_leaf;
  }
  return results;
}

}  // namespace minidb