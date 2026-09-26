#include "engine.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace minidb {
namespace {
std::string FormatDouble(double v) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(4) << v;
  return oss.str();
}
}  // namespace

QueryResult Engine::ExecuteCreateTable(const CreateTableStmt& stmt) {
  if (!catalog.CreateTable(stmt.table, stmt.schema)) {
    throw std::runtime_error("ExecuteCreateTable: Table " + stmt.table +
                             " already exists");
  }
  return QueryResult{};
}

QueryResult Engine::ExecuteCreateIndex(const CreateIndexStmt& stmt) {
  if (!catalog.CreateIndex(stmt.index_name, stmt.table, stmt.column)) {
    throw std::runtime_error(
        "ExecuteCreateIndex: CREATE INDEX " + stmt.index_name +
        " failed: Index already exists, table " + stmt.table + " or column " +
        stmt.column + " doesn't exist or column isn't an INT column");
  }
  return QueryResult{};
}

QueryResult Engine::ExecuteCreateVectorIndex(
    const CreateVectorIndexStmt& stmt) {
  if (!catalog.CreateVectorIndex(stmt.index_name, stmt.table, stmt.column,
                                 stmt.num_clusters)) {
    throw std::runtime_error(
        "ExecuteVectorCreateIndex: CREATE INDEX " + stmt.index_name +
        " failed: Index already exists, table " + stmt.table + " or column " +
        stmt.column + " doesn't exist or column isn't a VECTOR column");
  }
  return QueryResult{};
}

QueryResult Engine::ExecuteRefreshColumnar(const RefreshColumnarStmt& stmt,
                                           const TransactionManager& txn_mgr) {
  if (!catalog.HasTable(stmt.table))
    throw std::runtime_error("Unknown table " + stmt.table);
  catalog.RefreshColumnStore(stmt.table, txn_mgr);
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
  RID rid = heap->InsertTuple(txn, bytes);
  for (const auto& [column, tree] : catalog.GetIndexesForTable(stmt.table)) {
    int idx = schema->ColumnIndex(column);
    tree->Insert(stmt.values[static_cast<size_t>(idx)].int_val, rid);
  }
  for (const auto& [column, index] :
       catalog.GetVectorIndexesForTable(stmt.table)) {
    int idx = schema->ColumnIndex(column);
    index->Insert(stmt.values[static_cast<size_t>(idx)].vector_val, rid);
  }
  QueryResult result;
  result.rows_affected = 1;
  return result;
}

std::pair<std::unique_ptr<Executor>, std::string> Engine::BuildScanPlan(
    const SelectStmt& stmt, const Schema* schema, HeapFile* heap,
    TxnId reader_txn, const TransactionManager& txn_mgr) {
  BPlusTree* index = nullptr;
  Predicate indexed_pred;
  std::vector<Predicate> remaining_preds;
  for (auto& pred : stmt.where) {
    if (!index && pred.op == "=" && pred.value.type == ColumnType::INTEGER) {
      if (BPlusTree* t = catalog.GetIndex(stmt.table, pred.column)) {
        index = t;
        indexed_pred = pred;
        continue;
      }
    }
    remaining_preds.push_back(pred);
  }
  std::string explain;
  std::unique_ptr<Executor> plan;
  if (index) {
    explain = "IndexScan(" + stmt.table + "." + indexed_pred.column + " = " +
              indexed_pred.value.ToString() + ")";
    plan = std::make_unique<IndexScanExecutor>(
        index, indexed_pred.value.int_val, heap, schema, reader_txn, &txn_mgr);
  } else {
    explain = "SeqScan(" + stmt.table + ") [no matching index -> full scan]";
    plan =
        std::make_unique<SeqScanExecutor>(heap, schema, reader_txn, &txn_mgr);
  }
  if (!remaining_preds.empty()) {
    explain = "Filter(" + std::to_string(stmt.where.size()) +
              " predicates)\n " + explain;
    plan =
        std::make_unique<FilterExecutor>(std::move(plan), schema, stmt.where);
  }
  return {std::move(plan), explain};
}

QueryResult Engine::ExecuteAggregate(const AggregateSpec& agg,
                                     const SelectStmt& stmt,
                                     const Schema* schema, HeapFile* heap,
                                     TxnId reader_txn,
                                     const TransactionManager& txn_mgr) {
  if (agg.func != "COUNT") {
    int col_idx = schema->ColumnIndex(agg.column);
    if (col_idx < 0)
      throw std::runtime_error("ExecuteAggregate: Unknown column " +
                               agg.column);
    if (schema->columns[static_cast<size_t>(col_idx)].type !=
        ColumnType::INTEGER)
      throw std::runtime_error(agg.func + " requires an INT column. " +
                               agg.column + " is not one.");
  }
  QueryResult result;
  result.is_select = true;
  result.column_names = {agg.func + "(" + agg.column + ")"};
  ColumnStore* cs = catalog.GetColumnStore(stmt.table);
  bool use_columnar = stmt.where.empty() && cs != nullptr &&
                      (agg.func == "COUNT" || cs->HasColumn(agg.column));
  if (use_columnar) {
    result.explain = "ColumnarAggregate(" + agg.func + "(" + agg.column +
                     ")) [from REFESH COLUMNAR cache]";
    Value v;
    if (agg.func == "SUM") {
      int64_t x = 0;
      cs->Sum(agg.column, &x);
      v = Value::Int(x);
    } else if (agg.func == "AVG") {
      double x = 0;
      cs->Avg(agg.column, &x);
      v = Value::Text(FormatDouble(x));
    } else if (agg.func == "MIN") {
      int64_t x = 0;
      cs->Min(agg.column, &x);
      v = Value::Int(x);
    } else if (agg.func == "MAX") {
      int64_t x = 0;
      cs->Max(agg.column, &x);
      v = Value::Int(x);
    } else {
      v = Value::Int(static_cast<int64_t>(cs->RowCount()));
    }
    result.rows.push_back({v});
    return result;
  }
  auto [plan, plan_explain] =
      BuildScanPlan(stmt, schema, heap, reader_txn, txn_mgr);
  result.explain =
      "RowAggregate(" + agg.func + "(" + agg.column + "))\n " + plan_explain;
  int col_idx = (agg.func == "COUNT") ? -1 : schema->ColumnIndex(agg.column);
  plan->Open();
  std::vector<Value> row;
  int64_t count = 0, sum = 0, mn = 0, mx = 0;
  bool have_min_max = false;
  while (plan->Next(&row)) {
    count++;
    if (col_idx >= 0) {
      int64_t v = row[static_cast<size_t>(col_idx)].int_val;
      sum += v;
      if (!have_min_max) {
        mn = mx = v;
        have_min_max = true;
      } else {
        mn = std::min(mn, v);
        mx = std::max(mx, v);
      }
    }
  }
  plan->Close();

  Value v;
  if (agg.func == "COUNT") {
    v = Value::Int(count);
  } else if (count == 0) {
    throw std::runtime_error(agg.func + "(" + agg.column +
                             ")) over zero matching rows is undefined");
  } else if (agg.func == "SUM") {
    v = Value::Int(sum);
  } else if (agg.func == "AVG") {
    v = Value::Text(
        FormatDouble(static_cast<double>(sum) / static_cast<double>(count)));
  } else if (agg.func == "MIN") {
    v = Value::Int(mn);
  } else if (agg.func == "MAX") {
    v = Value::Int(mx);
  }
  result.rows.push_back({v});
  return result;
}

QueryResult Engine::ExecuteKNN(const KNNSpec& knn, const SelectStmt& stmt,
                               const Schema* schema, HeapFile* heap,
                               TxnId reader_txn,
                               const TransactionManager& txn_mgr) {
  IVFIndex* index = catalog.GetVectorIndex(stmt.table, knn.column);
  if (!index)
    throw std::runtime_error("No vector index on " + stmt.table + "." +
                             knn.column);
  if (knn.query_vector.size() != index->Dim())
    throw std::runtime_error("Query vector has dimension " +
                             std::to_string(knn.query_vector.size()) +
                             ", index expects " + std::to_string(index->Dim()));
  constexpr uint32_t overfetch_multiplier = 4;
  constexpr uint32_t nprobebase = 4;
  uint32_t fetch_count = knn.k * overfetch_multiplier;
  uint32_t nprobe = std::min(nprobebase, index->NumClusters());
  auto candidates = index->Search(knn.query_vector, fetch_count, nprobe);
  std::vector<std::string> out_columns = stmt.columns;
  if (out_columns.empty()) {
    for (const auto& c : schema->columns) out_columns.push_back(c.name);
  }
  std::vector<int> col_indices;
  for (const auto& c : out_columns) {
    int idx = schema->ColumnIndex(c);
    if (idx < 0)
      throw std::runtime_error("ExecuteKNN: Unknown column " + c +
                               " in SELECT list");
    col_indices.push_back(idx);
  }
  QueryResult result;
  result.is_select = true;
  result.column_names = out_columns;
  result.explain = "VectorIndexScan(" + stmt.table + "." + knn.column +
                   " <-> query, k=" + std::to_string(knn.k) +
                   ", nprobe=" + std::to_string(nprobe) + ")" +
                   (stmt.where.empty()
                        ? ""
                        : "\n PostFilter(" + std::to_string(stmt.where.size()) +
                              " predicates");
  for (const auto& rid : candidates) {
    if (result.rows.size() >= knn.k) break;
    TupleHeader hdr;
    std::vector<char> bytes;
    if (!heap->ReadTuple(rid, &hdr, &bytes)) continue;
    if (!txn_mgr.isVisible(hdr, reader_txn)) continue;
    auto values = DeserializeRow(*schema, bytes);
    if (!stmt.where.empty() && !EvaluatePredicate(*schema, values, stmt.where))
      continue;
    std::vector<Value> projected;
    for (int idx : col_indices)
      projected.push_back(values[static_cast<size_t>(idx)]);
    result.rows.push_back(std::move(projected));
  }
  return result;
}

QueryResult Engine::ExecuteSelect(const SelectStmt& stmt, Transaction* txn,
                                  const TransactionManager& txn_mgr) {
  const Schema* schema = catalog.GetSchema(stmt.table);
  if (!schema)
    throw std::runtime_error("ExecuteSelect: Unknown table " + stmt.table);
  HeapFile* heap = catalog.GetHeap(stmt.table);
  TxnId reader_txn = txn ? txn->id : INVALID_TXN_ID;

  if (stmt.aggregate) {
    return ExecuteAggregate(*stmt.aggregate, stmt, schema, heap, reader_txn,
                            txn_mgr);
  }
  if (stmt.knn) {
    return ExecuteKNN(*stmt.knn, stmt, schema, heap, reader_txn, txn_mgr);
  }

  auto [plan, base_explain] =
      BuildScanPlan(stmt, schema, heap, reader_txn, txn_mgr);
  std::vector<std::string> out_columns = stmt.columns;
  if (out_columns.empty())
    for (const auto& c : schema->columns) out_columns.push_back(c.name);
  std::string explain = "Project(" + std::to_string(out_columns.size()) +
                        " columns)\n " + base_explain;
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
  if (auto* s = std::get_if<CreateIndexStmt>(&stmt))
    return ExecuteCreateIndex(*s);
  if (auto* s = std::get_if<CreateVectorIndexStmt>(&stmt))
    return ExecuteCreateVectorIndex(*s);
  if (auto* s = std::get_if<RefreshColumnarStmt>(&stmt))
    return ExecuteRefreshColumnar(*s, txn_mgr);
  if (auto* s = std::get_if<InsertStmt>(&stmt)) return ExecuteInsert(*s, txn);
  if (auto* s = std::get_if<SelectStmt>(&stmt))
    return ExecuteSelect(*s, txn, txn_mgr);
  if (auto* s = std::get_if<DeleteStmt>(&stmt))
    return ExecuteDelete(*s, txn, txn_mgr);
  throw std::runtime_error("Execute: Unknown statement type");
}
}  // namespace minidb