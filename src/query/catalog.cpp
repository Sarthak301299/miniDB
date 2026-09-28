#include "catalog.h"

#include "columnar.h"

namespace minidb {

struct TableEntry {
  std::string name;
  Schema schema;
  std::vector<PageId> pages;
};

struct IndexEntry {
  std::string index_name;
  std::string table;
  std::string column;
  PageId root_page_id;
};

struct VectorIndexEntry {
  std::string index_name;
  std::string table;
  std::string column;
  uint32_t dim;
  std::vector<std::vector<float>> centroids;
  std::vector<std::vector<PageId>> cluster_pages;
};

void AppendU16(std::vector<char>* buf, uint16_t v) {
  const char* p = reinterpret_cast<const char*>(&v);
  buf->insert(buf->end(), p, p + sizeof(v));
}

void AppendU8(std::vector<char>* buf, uint8_t v) {
  buf->push_back(static_cast<char>(v));
}

void AppendU32(std::vector<char>* buf, uint32_t v) {
  const char* p = reinterpret_cast<const char*>(&v);
  buf->insert(buf->end(), p, p + sizeof(v));
}

void AppendFloat(std::vector<char>* buf, float v) {
  const char* p = reinterpret_cast<const char*>(&v);
  buf->insert(buf->end(), p, p + sizeof(v));
}

void AppendI64(std::vector<char>* buf, int64_t v) {
  const char* p = reinterpret_cast<const char*>(&v);
  buf->insert(buf->end(), p, p + sizeof(v));
}

void AppendString(std::vector<char>* buf, const std::string& s) {
  AppendU16(buf, static_cast<uint16_t>(s.size()));
  buf->insert(buf->end(), s.begin(), s.end());
}

std::vector<char> SerializeCatalog(
    const std::vector<TableEntry>& tables,
    const std::vector<IndexEntry>& indexes,
    const std::vector<VectorIndexEntry>& vector_indexes) {
  std::vector<char> out;
  AppendU16(&out, static_cast<uint16_t>(tables.size()));
  for (const auto& table : tables) {
    AppendString(&out, table.name);
    AppendU16(&out, static_cast<uint16_t>(table.schema.columns.size()));
    for (const auto& col : table.schema.columns) {
      AppendString(&out, col.name);
      AppendU8(&out, static_cast<uint8_t>(col.type));
      AppendU32(&out, col.dim);
    }
    AppendU16(&out, static_cast<uint16_t>(table.pages.size()));
    for (const auto& page_id : table.pages) AppendI64(&out, page_id);
  }
  AppendU16(&out, static_cast<uint16_t>(indexes.size()));
  for (const auto& idx : indexes) {
    AppendString(&out, idx.index_name);
    AppendString(&out, idx.table);
    AppendString(&out, idx.column);
    AppendI64(&out, idx.root_page_id);
  }
  AppendU16(&out, static_cast<uint16_t>(vector_indexes.size()));
  for (const auto& idx : vector_indexes) {
    AppendString(&out, idx.index_name);
    AppendString(&out, idx.table);
    AppendString(&out, idx.column);
    AppendU32(&out, idx.dim);
    AppendU16(&out, idx.centroids.size());
    for (uint16_t i = 0; i < idx.centroids.size(); ++i) {
      for (const auto& f : idx.centroids[i]) AppendFloat(&out, f);
      AppendU16(&out, static_cast<uint16_t>(idx.cluster_pages[i].size()));
      for (const auto& pid : idx.cluster_pages[i]) AppendI64(&out, pid);
    }
  }
  return out;
}

class Reader {
 private:
  const char* data;
  size_t len;
  size_t pos = 0;
  void Need(size_t n) {
    if (pos + n > len)
      throw std::runtime_error(
          "Catalog: corrupt catalog page (truncated read)");
  }

 public:
  Reader(const char* data, size_t len) : data(data), len(len) {}

  uint16_t ReadU16() {
    Need(sizeof(uint16_t));
    uint16_t v;
    std::memcpy(&v, data + pos, sizeof(v));
    pos += sizeof(v);
    return v;
  }

  uint8_t ReadU8() {
    Need(sizeof(uint8_t));
    return static_cast<uint8_t>(data[pos++]);
  }

  uint16_t ReadU32() {
    Need(sizeof(uint32_t));
    uint32_t v;
    std::memcpy(&v, data + pos, sizeof(v));
    pos += sizeof(v);
    return v;
  }

  uint16_t ReadFloat() {
    Need(sizeof(float));
    float v;
    std::memcpy(&v, data + pos, sizeof(v));
    pos += sizeof(v);
    return v;
  }

  int64_t ReadI64() {
    Need(sizeof(int64_t));
    int64_t v;
    std::memcpy(&v, data + pos, sizeof(v));
    pos += sizeof(v);
    return v;
  }

  std::string ReadString() {
    uint16_t n = ReadU16();
    Need(n);
    std::string s(data + pos, data + pos + n);
    pos += n;
    return s;
  }
};

struct DeserializedCatalog {
  std::vector<TableEntry> tables;
  std::vector<IndexEntry> indexes;
  std::vector<VectorIndexEntry> vector_indexes;
};

DeserializedCatalog DeserializeCatalog(const char* data, size_t len) {
  Reader r(data, len);
  DeserializedCatalog out;
  uint16_t num_tables = r.ReadU16();
  out.tables.reserve(num_tables);
  for (uint16_t i = 0; i < num_tables; ++i) {
    TableEntry t;
    t.name = r.ReadString();
    uint16_t num_cols = r.ReadU16();
    for (uint16_t c = 0; c < num_cols; ++c) {
      Column col;
      col.name = r.ReadString();
      col.type = static_cast<ColumnType>(r.ReadU8());
      col.dim = r.ReadU32();
      t.schema.name_to_index[col.name] =
          static_cast<uint64_t>(t.schema.columns.size());
      t.schema.columns.push_back(col);
    }
    uint16_t num_pages = r.ReadU16();
    for (uint16_t p = 0; p < num_pages; ++p) t.pages.push_back(r.ReadI64());
    out.tables.push_back(std::move(t));
  }
  uint16_t num_indexes = r.ReadU16();
  out.indexes.reserve(num_indexes);
  for (uint16_t i = 0; i < num_indexes; ++i) {
    IndexEntry idx;
    idx.index_name = r.ReadString();
    idx.table = r.ReadString();
    idx.column = r.ReadString();
    idx.root_page_id = r.ReadI64();
    out.indexes.push_back(std::move(idx));
  }
  uint16_t num_vector_indexes = r.ReadU16();
  out.vector_indexes.reserve(num_vector_indexes);
  for (uint16_t i = 0; i < num_vector_indexes; ++i) {
    VectorIndexEntry vidx;
    vidx.index_name = r.ReadString();
    vidx.table = r.ReadString();
    vidx.column = r.ReadString();
    vidx.dim = r.ReadU32();
    uint16_t num_clusters = r.ReadU16();
    vidx.centroids.resize(num_clusters);
    vidx.cluster_pages.resize(num_clusters);
    for (uint16_t i = 0; i < num_clusters; ++i) {
      vidx.centroids[i].resize(vidx.dim);
      for (uint32_t d = 0; d < vidx.dim; ++d)
        vidx.centroids[i][d] = r.ReadFloat();
      uint16_t num_pages = r.ReadU16();
      for (uint16_t p = 0; p < num_pages; ++p)
        vidx.cluster_pages[i].push_back(r.ReadI64());
    }
    out.vector_indexes.push_back(std::move(vidx));
  }
  return out;
}

Catalog::Catalog(DiskManager* disk, BufferPool* pool, WALManager* wal)
    : disk(disk), pool(pool), wal(wal) {
  disk->AdvancePastPage(catalogPageId);
  LoadFromDisk();
}

void Catalog::LoadFromDisk() {
  Page* page = pool->FetchPage(catalogPageId);
  uint16_t nt;
  std::memcpy(&nt, page->GetData() + Page::HeaderSize(), sizeof(nt));
  auto parsed = DeserializeCatalog(page->GetData() + Page::HeaderSize(),
                                   Page::UsableSize());
  pool->UnpinPage(catalogPageId, false);
  for (const auto& e : parsed.tables) {
    TableInfo info;
    info.schema = e.schema;
    info.heap = std::make_unique<HeapFile>(pool, wal, e.pages);
    WireHeapCallback(info.heap.get());
    tables.emplace(e.name, std::move(info));
  }
  for (const auto& e : parsed.indexes) {
    IndexInfo info;
    info.table = e.table;
    info.column = e.column;
    info.tree = std::make_unique<BPlusTree>(pool, wal, e.root_page_id);
    WireTreeCallback(info.tree.get());
    indexes.emplace(e.index_name, std::move(info));
  }
  for (const auto& e : parsed.vector_indexes) {
    VectorIndexInfo info;
    info.table = e.table;
    info.column = e.column;
    info.index = std::make_unique<IVFIndex>(pool, wal, e.dim, e.centroids,
                                            e.cluster_pages);
    WireVectorIndexCallback(info.index.get());
    vector_indexes.emplace(e.index_name, std::move(info));
  }
}

void Catalog::ReloadFromDisk() {
  std::lock_guard<std::mutex> lock(latch);
  pool->InvalidatePage(catalogPageId);
  tables.clear();
  indexes.clear();
  vector_indexes.clear();
  LoadFromDisk();
}

void Catalog::WireHeapCallback(HeapFile* heap) {
  heap->SetOnNewPageCallback([this](PageId) { this->Persist(); });
}

void Catalog::WireTreeCallback(BPlusTree* tree) {
  tree->SetOnRootChangeCallback([this](PageId) { this->Persist(); });
}

void Catalog::WireVectorIndexCallback(IVFIndex* index) {
  index->SetOnNewPageCallback(
      [this](size_t, const std::vector<PageId>&) { this->Persist(); });
}

void Catalog::Persist() {
  std::lock_guard<std::mutex> lock(latch);
  PersistLocked();
}

void Catalog::PersistLocked() {
  std::vector<TableEntry> table_entries;
  table_entries.reserve(tables.size());
  for (const auto& [name, info] : tables) {
    TableEntry e;
    e.name = name;
    e.schema = info.schema;
    e.pages = info.heap->GetAllPageIds();
    table_entries.push_back(std::move(e));
  }

  std::vector<IndexEntry> index_entries;
  index_entries.reserve(indexes.size());
  for (const auto& [name, info] : indexes) {
    IndexEntry e;
    e.index_name = name;
    e.table = info.table;
    e.column = info.column;
    e.root_page_id = info.tree->RootPageId();
    index_entries.push_back(std::move(e));
  }
  std::vector<VectorIndexEntry> vector_index_entries;
  vector_index_entries.reserve(vector_indexes.size());
  for (const auto& [name, info] : vector_indexes) {
    VectorIndexEntry e;
    e.index_name = name;
    e.table = info.table;
    e.column = info.column;
    e.dim = info.index->Dim();
    e.centroids = info.index->Centroids();
    e.cluster_pages = info.index->AllClusterPages();
    vector_index_entries.push_back(std::move(e));
  }
  auto bytes =
      SerializeCatalog(table_entries, index_entries, vector_index_entries);
  if (bytes.size() > Page::UsableSize()) {
    throw std::runtime_error(
        "Catalog: serialized catalog (" + std::to_string(bytes.size()) +
        " bytes) exceeds the single catalog page capacity (" +
        std::to_string(Page::UsableSize()) + " bytes)");
  }
  Page* page = pool->FetchPage(catalogPageId);
  char* base = page->GetData() + Page::HeaderSize();
  std::memcpy(base, bytes.data(), bytes.size());
  std::memset(base + bytes.size(), 0, Page::UsableSize() - bytes.size());

  WALRecord record;
  record.type = WALRecordType::UPDATE;
  record.txn_id = INVALID_TXN_ID;
  record.page_id = catalogPageId;
  record.offset = 0;
  record.after_image.assign(base, base + Page::UsableSize());
  LSN lsn = wal->Append(record);
  page->SetLSN(lsn);
  pool->UnpinPage(catalogPageId, true);
}

bool Catalog::CreateTable(const std::string& name, const Schema& schema) {
  std::lock_guard<std::mutex> lock(latch);
  if (tables.contains(name)) return false;
  TableInfo info;
  info.schema = schema;
  info.heap = std::make_unique<HeapFile>(pool, wal);
  WireHeapCallback(info.heap.get());
  tables.emplace(name, std::move(info));
  PersistLocked();
  return true;
}

bool Catalog::CreateIndex(const std::string& index_name,
                          const std::string& table, const std::string& column) {
  std::shared_ptr<HeapFile> heap;
  Schema schema;
  {
    std::lock_guard<std::mutex> lock(latch);
    if (indexes.contains(index_name)) return false;
    auto it = tables.find(table);
    if (it == tables.end()) return false;
    int col_idx = it->second.schema.ColumnIndex(column);
    if (col_idx < 0 ||
        it->second.schema.columns[static_cast<size_t>(col_idx)].type !=
            ColumnType::INTEGER)
      return false;
    heap = it->second.heap;
    schema = it->second.schema;
  }
  int col_idx = schema.ColumnIndex(column);
  auto tree = std::make_shared<BPlusTree>(pool, wal);
  for (const auto& [rid, bytes] : heap->ScanAllPhysical()) {
    auto values = DeserializeRow(schema, bytes);
    tree->Insert(values[static_cast<size_t>(col_idx)].int_val, rid);
  }
  std::lock_guard<std::mutex> lock(latch);
  if (indexes.contains(index_name)) return false;
  WireTreeCallback(tree.get());
  IndexInfo info;
  info.table = table;
  info.column = column;
  info.tree = std::move(tree);
  indexes.emplace(index_name, std::move(info));
  PersistLocked();
  return true;
}

std::shared_ptr<HeapFile> Catalog::GetHeap(const std::string& name) {
  std::lock_guard<std::mutex> lock(latch);
  auto it = tables.find(name);
  if (it != tables.end()) return it->second.heap;
  return nullptr;
}

std::optional<Schema> Catalog::GetSchema(const std::string& name) {
  std::lock_guard<std::mutex> lock(latch);
  auto it = tables.find(name);
  if (it != tables.end()) return it->second.schema;
  return std::nullopt;
}

bool Catalog::HasTable(const std::string& name) const {
  std::lock_guard<std::mutex> lock(latch);
  return tables.contains(name);
}

std::shared_ptr<BPlusTree> Catalog::GetIndex(const std::string& table,
                                             const std::string& column) {
  std::lock_guard<std::mutex> lock(latch);
  for (const auto& [name, info] : indexes) {
    if (info.table == table && info.column == column) return info.tree;
  }
  return nullptr;
}

std::vector<std::pair<std::string, std::shared_ptr<BPlusTree>>>
Catalog::GetIndexesForTable(const std::string& table) {
  std::lock_guard<std::mutex> lock(latch);
  std::vector<std::pair<std::string, std::shared_ptr<BPlusTree>>> out;
  for (const auto& [name, info] : indexes) {
    if (info.table == table) out.push_back({info.column, info.tree});
  }
  return out;
}

void Catalog::RefreshColumnStore(const std::string& table,
                                 const TransactionManager& txn_mgr) {
  std::shared_ptr<HeapFile> heap;
  Schema schema;
  std::shared_ptr<ColumnStore> store;
  {
    std::lock_guard<std::mutex> lock(latch);
    auto it = tables.find(table);
    if (it == tables.end()) throw std::runtime_error("Unknown table " + table);
    if (!it->second.column_store) {
      it->second.column_store = std::make_shared<ColumnStore>();
    }
    heap = it->second.heap;
    schema = it->second.schema;
    store = it->second.column_store;
  }
  store->Refresh(heap.get(), schema, txn_mgr);
}

std::shared_ptr<ColumnStore> Catalog::GetColumnStore(const std::string& table) {
  std::lock_guard<std::mutex> lock(latch);
  auto it = tables.find(table);
  if (it != tables.end()) return it->second.column_store;
  return nullptr;
}

bool Catalog::CreateVectorIndex(const std::string& index_name,
                                const std::string& table,
                                const std::string& column,
                                uint32_t num_clusters) {
  std::shared_ptr<HeapFile> heap;
  Schema schema;
  uint32_t dim = 0;
  {
    std::lock_guard<std::mutex> lock(latch);
    if (vector_indexes.contains(index_name)) return false;
    auto it = tables.find(table);
    if (it == tables.end()) return false;
    int col_idx = it->second.schema.ColumnIndex(column);
    if (col_idx < 0 ||
        it->second.schema.columns[static_cast<size_t>(col_idx)].type !=
            ColumnType::VECTOR)
      return false;
    heap = it->second.heap;
    schema = it->second.schema;
    dim = it->second.schema.columns[static_cast<size_t>(col_idx)].dim;
  }
  int col_idx = schema.ColumnIndex(column);
  std::vector<std::pair<std::vector<float>, RID>> vectors;
  for (const auto& [rid, bytes] : heap->ScanAllPhysical()) {
    auto values = DeserializeRow(schema, bytes);
    vectors.push_back({values[static_cast<size_t>(col_idx)].vector_val, rid});
  }
  auto index =
      std::make_shared<IVFIndex>(pool, wal, dim, num_clusters, vectors);
  std::lock_guard<std::mutex> lock(latch);
  if (vector_indexes.contains(index_name)) return false;
  WireVectorIndexCallback(index.get());
  VectorIndexInfo info;
  info.table = table;
  info.column = column;
  info.index = std::move(index);
  vector_indexes.emplace(index_name, std::move(info));
  PersistLocked();
  return true;
}

std::shared_ptr<IVFIndex> Catalog::GetVectorIndex(const std::string& table,
                                                  const std::string& column) {
  std::lock_guard<std::mutex> lock(latch);
  for (const auto& [name, info] : vector_indexes) {
    if (info.table == table && info.column == column) return info.index;
  }
  return nullptr;
}

std::vector<std::pair<std::string, std::shared_ptr<IVFIndex>>>
Catalog::GetVectorIndexesForTable(const std::string& table) {
  std::lock_guard<std::mutex> lock(latch);
  std::vector<std::pair<std::string, std::shared_ptr<IVFIndex>>> out;
  for (const auto& [name, info] : vector_indexes) {
    if (info.table == table) out.push_back({info.column, info.index});
  }
  return out;
}

}  // namespace minidb