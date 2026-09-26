#pragma once

#include <vector>

#include "catalog.h"
#include "executor.h"
#include "parser.h"

namespace minidb {

struct QueryResult {
  bool is_select = false;
  std::vector<std::string> column_names;
  std::vector<std::vector<Value>> rows;
  size_t rows_affected = 0;
  std::string explain;
};

class Engine {
 private:
  DiskManager* disk;
  BufferPool* pool;
  WALManager* wal;
  Catalog catalog;

  std::pair<std::unique_ptr<Executor>, std::string> BuildScanPlan(
      const SelectStmt& stmt, const Schema* schema, HeapFile* heap,
      TxnId reader_txn, const TransactionManager& txn_mgr);
  QueryResult ExecuteCreateTable(const CreateTableStmt& stmt);
  QueryResult ExecuteCreateIndex(const CreateIndexStmt& stmt);
  QueryResult ExecuteCreateVectorIndex(const CreateVectorIndexStmt& stmt);
  QueryResult ExecuteRefreshColumnar(const RefreshColumnarStmt& stmt,
                                     const TransactionManager& txn_mgr);
  QueryResult ExecuteInsert(const InsertStmt& stmt, Transaction* txn);
  QueryResult ExecuteSelect(const SelectStmt& stmt, Transaction* txn,
                            const TransactionManager& txn_mgr);
  QueryResult ExecuteDelete(const DeleteStmt& stmt, Transaction* txn,
                            const TransactionManager& txn_mgr);
  QueryResult ExecuteAggregate(const AggregateSpec& agg, const SelectStmt& stmt,
                               const Schema* schema, HeapFile* heap,
                               TxnId reader_txn,
                               const TransactionManager& txn_mgr);
  QueryResult ExecuteKNN(const KNNSpec& knn, const SelectStmt& stmt,
                         const Schema* schema, HeapFile* heap, TxnId reader_txn,
                         const TransactionManager& txn_mgr);

 public:
  Engine(DiskManager* disk, BufferPool* pool, WALManager* wal);
  Catalog& GetCatalog() { return catalog; }
  QueryResult Execute(const std::string& sql, Transaction* txn,
                      const TransactionManager& txn_mgr);
};

}  // namespace minidb