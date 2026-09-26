#pragma once
#include <memory>
#include <mutex>

#include "btree.h"
#include "buffer_pool.h"
#include "columnar.h"
#include "heap_file.h"
#include "ivf_index.h"
#include "value.h"
#include "wal_manager.h"

namespace minidb {
class Catalog {
 private:
  struct TableInfo {
    Schema schema;
    std::unique_ptr<HeapFile> heap;
    std::unique_ptr<ColumnStore> column_store;
  };
  struct IndexInfo {
    std::string table;
    std::string column;
    std::unique_ptr<BPlusTree> tree;
  };
  struct VectorIndexInfo {
    std::string table;
    std::string column;
    std::unique_ptr<IVFIndex> index;
  };
  DiskManager* disk;
  BufferPool* pool;
  WALManager* wal;
  mutable std::mutex latch;
  std::unordered_map<std::string, TableInfo> tables;
  std::unordered_map<std::string, IndexInfo> indexes;
  std::unordered_map<std::string, VectorIndexInfo> vector_indexes;
  static constexpr PageId catalogPageId = 0;
  void LoadFromDisk();
  void WireHeapCallback(HeapFile* heap);
  void WireTreeCallback(BPlusTree* tree);
  void WireVectorIndexCallback(IVFIndex* index);
  void Persist();
  void PersistLocked();

 public:
  Catalog(DiskManager* disk, BufferPool* pool, WALManager* wal);
  bool CreateTable(const std::string& name, const Schema& schema);
  HeapFile* GetHeap(const std::string& name);
  const Schema* GetSchema(const std::string& name);
  bool HasTable(const std::string& name) const;
  bool CreateIndex(const std::string& index_name, const std::string& table,
                   const std::string& column);
  bool CreateVectorIndex(const std::string& index_name,
                         const std::string& table, const std::string& column,
                         uint32_t num_clusters);
  IVFIndex* GetVectorIndex(const std::string& table, const std::string& column);
  BPlusTree* GetIndex(const std::string& table, const std::string& column);
  std::vector<std::pair<std::string, IVFIndex*>> GetVectorIndexesForTable(
      const std::string& table);
  std::vector<std::pair<std::string, BPlusTree*>> GetIndexesForTable(
      const std::string& table);
  void RefreshColumnStore(const std::string& table,
                          const TransactionManager& txn_mgr);
  ColumnStore* GetColumnStore(const std::string& table);
};
}  // namespace minidb