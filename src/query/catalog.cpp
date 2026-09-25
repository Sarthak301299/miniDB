#include "catalog.h"

namespace minidb {

struct TableEntry {
  std::string name;
  Schema schema;
  std::vector<PageId> pages;
};

void AppendU16(std::vector<char>* buf, uint16_t v) {
  const char* p = reinterpret_cast<const char*>(&v);
  buf->insert(buf->end(), p, p + sizeof(v));
}

void AppendU8(std::vector<char>* buf, uint8_t v) {
  buf->push_back(static_cast<char>(v));
}

void AppendI64(std::vector<char>* buf, int64_t v) {
  const char* p = reinterpret_cast<const char*>(&v);
  buf->insert(buf->end(), p, p + sizeof(v));
}

void AppendString(std::vector<char>* buf, const std::string& s) {
  AppendU16(buf, static_cast<uint16_t>(s.size()));
  buf->insert(buf->end(), s.begin(), s.end());
}

std::vector<char> SerializeCatalog(const std::vector<TableEntry>& tables) {
  std::vector<char> out;
  AppendU16(&out, static_cast<uint16_t>(tables.size()));
  for (const auto& table : tables) {
    AppendString(&out, table.name);
    AppendU16(&out, static_cast<uint16_t>(table.schema.columns.size()));
    for (const auto& col : table.schema.columns) {
      AppendString(&out, col.name);
      AppendU8(&out, static_cast<uint8_t>(col.type));
    }
    AppendU16(&out, static_cast<uint16_t>(table.pages.size()));
    for (const auto& page_id : table.pages) AppendI64(&out, page_id);
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

std::vector<TableEntry> DeserializeCatalog(const char* data, size_t len) {
  Reader r(data, len);
  std::vector<TableEntry> tables;
  uint16_t num_tables = r.ReadU16();
  tables.reserve(num_tables);
  for (uint16_t i = 0; i < num_tables; ++i) {
    TableEntry t;
    t.name = r.ReadString();
    uint16_t num_cols = r.ReadU16();
    for (uint16_t c = 0; c < num_cols; ++c) {
      Column col;
      col.name = r.ReadString();
      col.type = static_cast<ColumnType>(r.ReadU8());
      t.schema.columns.push_back(col);
    }
    uint16_t num_pages = r.ReadU16();
    for (uint16_t p = 0; p < num_pages; ++p) t.pages.push_back(r.ReadI64());
    tables.push_back(std::move(t));
  }
  return tables;
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
  auto entries = DeserializeCatalog(page->GetData() + Page::HeaderSize(),
                                    Page::UsableSize());
  pool->UnpinPage(catalogPageId, false);
  for (const auto& e : entries) {
    TableInfo info;
    info.schema = e.schema;
    info.heap = std::make_unique<HeapFile>(pool, wal, e.pages);
    WireCallback(info.heap.get());
    tables.emplace(e.name, std::move(info));
  }
}

void Catalog::WireCallback(HeapFile* heap) {
  heap->SetOnNewPageCallback([this](PageId) { this->Persist(); });
}

void Catalog::Persist() {
  std::lock_guard<std::mutex> lock(latch);
  PersistLocked();
}

void Catalog::PersistLocked() {
  std::vector<TableEntry> entries;
  entries.reserve(tables.size());
  for (const auto& [name, info] : tables) {
    TableEntry e;
    e.name = name;
    e.schema = info.schema;
    e.pages = info.heap->GetAllPageIds();
    entries.push_back(std::move(e));
  }

  auto bytes = SerializeCatalog(entries);
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
  wal->Flush(lsn);
  pool->UnpinPage(catalogPageId, true);
}

bool Catalog::CreateTable(const std::string& name, const Schema& schema) {
  std::lock_guard<std::mutex> lock(latch);
  if (tables.contains(name)) return false;
  TableInfo info;
  info.schema = schema;
  info.heap = std::make_unique<HeapFile>(pool, wal);
  WireCallback(info.heap.get());
  tables.emplace(name, std::move(info));
  PersistLocked();
  return true;
}

HeapFile* Catalog::GetHeap(const std::string& name) {
  std::lock_guard<std::mutex> lock(latch);
  auto it = tables.find(name);
  if (it != tables.end())
    return it->second.heap.get();
  else
    return nullptr;
}

const Schema* Catalog::GetSchema(const std::string& name) {
  std::lock_guard<std::mutex> lock(latch);
  auto it = tables.find(name);
  if (it != tables.end())
    return &it->second.schema;
  else
    return nullptr;
}

bool Catalog::HasTable(const std::string& name) const {
  std::lock_guard<std::mutex> lock(latch);
  return tables.contains(name);
}
}  // namespace minidb