#include "engine.h"

namespace minidb {
QueryResult Engine::ExecuteCreateTable(const CreateTableStmt& stmt) {
  if (!catalog.CreateTable(stmt.table, stmt.schema)) {
    throw std::runtime_error("ExecuteCreateTable: Table " + stmt.table +
                             " already exists");
  }
  return QueryResult{};
}

QueryResult Engine::ExecuteInsert(const InsertStmt& stmt, Transaction* txn) {
  if (!txn)
    throw std::runtime_error(
        "ExecuteInsert: INSERT requires an active transaction");
  const Schema* schema = catalog.GetSchema(stmt.table);
  if (!schema)
    throw std::runtime_error("ExecuteInsert: Unknown table " + stmt.table);
  HeapFile* heap = catalog.GetHeap(stmt.table);
  auto bytes = SerializeRow(*schema, stmt.values);
  heap->InsertTuple(txn, bytes);
  QueryResult result;
  result.rows_affected = 1;
  return result;
}

QueryResult Engine::ExecuteSelect(const SelectStmt& stmt, Transaction* txn,
                                  const TransactionManager& txn_mgr) {
  const Schema* schema = catalog.GetSchema(stmt.table);
  if (!schema)
    throw std::runtime_error("ExecuteSelect: Unknown table " + stmt.table);
  HeapFile* heap = catalog.GetHeap(stmt.table);
  TxnId reader_txn = txn ? txn->id : INVALID_TXN_ID;
  std::string explain =
      "SeqScan(" + stmt.table + ") [no index available -> full scan]";
  std::unique_ptr<Executor> plan =
      std::make_unique<SeqScanExecutor>(heap, schema, reader_txn, &txn_mgr);
  if (!stmt.where.empty()) {
    explain = "Filter(" + std::to_string(stmt.where.size()) +
              " predicates)\n " + explain;
    plan =
        std::make_unique<FilterExecutor>(std::move(plan), schema, stmt.where);
  }
  std::vector<std::string> out_columns = stmt.columns;
  if (out_columns.empty())
    for (const auto& c : schema->columns) out_columns.push_back(c.name);
  explain = "Project(" + std::to_string(out_columns.size()) + " columns)\n " +
            explain;
  plan = std::make_unique<ProjectionExecutor>(std::move(plan), schema,
                                              stmt.columns);
  QueryResult result;
  result.explain = explain;
  result.is_select = true;
  result.column_names = out_columns;
  plan->Open();
  std::vector<Value> row;
  while (plan->Next(&row)) result.rows.push_back(row);
  plan->Close();
  return result;
}

QueryResult Engine::ExecuteDelete(const DeleteStmt& stmt, Transaction* txn,
                                  const TransactionManager& txn_mgr) {
  if (!txn)
    throw std::runtime_error(
        "ExecuteDelete: DELETE requires an active transaction");
  const Schema* schema = catalog.GetSchema(stmt.table);
  if (!schema)
    throw std::runtime_error("ExecuteSelect: Unknown table " + stmt.table);
  HeapFile* heap = catalog.GetHeap(stmt.table);
  auto rows = heap->Scan(txn->id, txn_mgr);
  size_t deleted = 0;
  for (const auto& [rid, rawrow] : rows) {
    auto row = DeserializeRow(*schema, rawrow);
    if (EvaluatePredicate(*schema, row, stmt.where)) {
      heap->DeleteTuple(txn, rid);
      deleted++;
    }
  }
  QueryResult result;
  result.rows_affected = deleted;
  return result;
}

Engine::Engine(DiskManager* disk, BufferPool* pool, WALManager* wal)
    : disk(disk), pool(pool), wal(wal), catalog(disk, pool, wal) {}

QueryResult Engine::Execute(const std::string& sql, Transaction* txn,
                            const TransactionManager& txn_mgr) {
  Statement stmt = ParseStatement(sql);
  if (auto* s = std::get_if<CreateTableStmt>(&stmt))
    return ExecuteCreateTable(*s);
  if (auto* s = std::get_if<InsertStmt>(&stmt)) return ExecuteInsert(*s, txn);
  if (auto* s = std::get_if<SelectStmt>(&stmt))
    return ExecuteSelect(*s, txn, txn_mgr);
  if (auto* s = std::get_if<DeleteStmt>(&stmt))
    return ExecuteDelete(*s, txn, txn_mgr);
  throw std::runtime_error("Execute: Unknown statement type");
}
}  // namespace minidb