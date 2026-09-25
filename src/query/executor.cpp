#include "executor.h"

#include <stdexcept>

namespace minidb {
SeqScanExecutor::SeqScanExecutor(HeapFile* heap, const Schema* schema,
                                 TxnId reader_txn,
                                 const TransactionManager* txn_mgr)
    : heap(heap), schema(schema), reader_txn(reader_txn), txn_mgr(txn_mgr) {}

void SeqScanExecutor::Open() {
  rows = heap->Scan(reader_txn, *txn_mgr);
  pos = 0;
}

bool SeqScanExecutor::Next(std::vector<Value>* out_row) {
  if (pos >= rows.size()) return false;
  *out_row = DeserializeRow(*schema, rows[pos].second);
  pos++;
  return true;
}

void SeqScanExecutor::Close() { rows.clear(); }

FilterExecutor::FilterExecutor(std::unique_ptr<Executor> child,
                               const Schema* schema,
                               std::vector<Predicate> predicates)
    : child(std::move(child)),
      schema(schema),
      predicates(std::move(predicates)) {}

void FilterExecutor::Open() { child->Open(); }

bool FilterExecutor::Next(std::vector<Value>* out_row) {
  std::vector<Value> row;
  while (child->Next(&row)) {
    if (EvaluatePredicate(*schema, row, predicates)) {
      *out_row = std::move(row);
      return true;
    }
  }
  return false;
}

void FilterExecutor::Close() { child->Close(); }

ProjectionExecutor::ProjectionExecutor(std::unique_ptr<Executor> child,
                                       const Schema* schema,
                                       std::vector<std::string> columns)
    : child(std::move(child)) {
  if (columns.empty()) {
    for (size_t i = 0; i < schema->columns.size(); ++i) {
      colidx.push_back(static_cast<int>(i));
    }
  }
  for (const auto& col : columns) {
    int idx = schema->ColumnIndex(col);
    if (idx < 0)
      throw std::runtime_error("ProjectionExecutor: Unknown column " + col +
                               " in SELECT list");
    colidx.push_back(idx);
  }
}

void ProjectionExecutor::Open() { child->Open(); }

bool ProjectionExecutor::Next(std::vector<Value>* out_row) {
  std::vector<Value> row;
  if (!child->Next(&row)) return false;
  out_row->clear();
  for (const auto& idx : colidx)
    out_row->push_back(row[static_cast<size_t>(idx)]);
  return true;
}

void ProjectionExecutor::Close() { child->Close(); }
}  // namespace minidb