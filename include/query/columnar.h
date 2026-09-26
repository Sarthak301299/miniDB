#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "heap_file.h"
#include "transaction_manager.h"
#include "value.h"

namespace minidb {
class ColumnStore {
 private:
  mutable std::mutex latch;
  std::unordered_map<std::string, std::vector<int64_t>> columns;
  size_t row_count = 0;

 public:
  void Refresh(HeapFile* heap, const Schema& schema,
               const TransactionManager& txn_mgr);
  bool HasColumn(const std::string& column) const;
  size_t RowCount() const;
  bool Sum(const std::string& column, int64_t* out) const;
  bool Avg(const std::string& column, double* out) const;
  bool Min(const std::string& column, int64_t* out) const;
  bool Max(const std::string& column, int64_t* out) const;
};
}  // namespace minidb