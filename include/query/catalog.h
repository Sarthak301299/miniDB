#pragma once
#include <memory>
#include <mutex>

#include "buffer_pool.h"
#include "heap_file.h"
#include "value.h"
#include "wal_manager.h"

namespace minidb {
class Catalog {
 private:
  struct TableInfo {
    Schema schema;
    std::unique_ptr<HeapFile> heap;
  };
  DiskManager* disk;
  BufferPool* pool;
  WALManager* wal;
  mutable std::mutex latch;
  std::unordered_map<std::string, TableInfo> tables;
  static constexpr PageId catalogPageId = 0;
  void LoadFromDisk();
  void WireCallback(HeapFile* heap);
  void Persist();
  void PersistLocked();

 public:
  Catalog(DiskManager* disk, BufferPool* pool, WALManager* wal);
  bool CreateTable(const std::string& name, const Schema& schema);
  HeapFile* GetHeap(const std::string& name);
  const Schema* GetSchema(const std::string& name);
  bool HasTable(const std::string& name) const;
};
}  // namespace minidb