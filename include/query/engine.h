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
  QueryResult ExecuteCreateTable(const CreateTableStmt& stmt);
  QueryResult ExecuteInsert(const InsertStmt& stmt, Transaction* txn);
  QueryResult ExecuteSelect(const SelectStmt& stmt, Transaction* txn,
                            const TransactionManager& txn_mgr);
  QueryResult ExecuteDelete(const DeleteStmt& stmt, Transaction* txn,
                            const TransactionManager& txn_mgr);

 public:
  Engine(DiskManager* disk, BufferPool* pool, WALManager* wal);
  Catalog& GetCatalog() { return catalog; }
  QueryResult Execute(const std::string& sql, Transaction* txn,
                      const TransactionManager& txn_mgr);
};

}  // namespace minidb