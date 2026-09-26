#include "columnar.h"

#include <algorithm>
#include <stdexcept>

namespace minidb {
void ColumnStore::Refresh(HeapFile* heap, const Schema& schema,
                          const TransactionManager& txn_mgr) {
  std::unordered_map<std::string, std::vector<int64_t>> new_columns;
  for (const auto& col : schema.columns) {
    if (col.type == ColumnType::INTEGER) new_columns[col.name] = {};
  }
  auto rows = heap->Scan(INVALID_TXN_ID, txn_mgr);
  for (auto& [_, bytes] : rows) {
    auto values = DeserializeRow(schema, bytes);
    for (size_t i = 0; i < schema.columns.size(); ++i) {
      if (schema.columns[i].type == ColumnType::INTEGER) {
        new_columns[schema.columns[i].name].push_back(values[i].int_val);
      }
    }
  }
  std::lock_guard<std::mutex> lock(latch);
  columns = std::move(new_columns);
  row_count = rows.size();
}

bool ColumnStore::HasColumn(const std::string& column) const {
  std::lock_guard<std::mutex> lock(latch);
  return columns.contains(column);
}

size_t ColumnStore::RowCount() const {
  std::lock_guard<std::mutex> lock(latch);
  return row_count;
}

bool ColumnStore::Sum(const std::string& column, int64_t* out) const {
  std::lock_guard<std::mutex> lock(latch);
  auto it = columns.find(column);
  if (it == columns.end()) return false;
  if (row_count == 0)
    throw std::runtime_error("ColumnStore: SUM(" + column +
                             ") over zero rows is undefined");
  const std::vector<int64_t>& data = it->second;
  int64_t total = 0;
  for (int64_t v : data) total += v;
  *out = total;
  return true;
}

bool ColumnStore::Avg(const std::string& column, double* out) const {
  std::lock_guard<std::mutex> lock(latch);
  auto it = columns.find(column);
  if (it == columns.end()) return false;
  if (row_count == 0)
    throw std::runtime_error("ColumnStore: AVG(" + column +
                             ") over zero rows is undefined");
  const std::vector<int64_t>& data = it->second;
  int64_t total = 0;
  for (int64_t v : data) total += v;
  *out = static_cast<double>(total) / static_cast<double>(row_count);
  return true;
}

bool ColumnStore::Min(const std::string& column, int64_t* out) const {
  std::lock_guard<std::mutex> lock(latch);
  auto it = columns.find(column);
  if (it == columns.end()) return false;
  if (row_count == 0)
    throw std::runtime_error("ColumnStore: MIN(" + column +
                             ") over zero rows is undefined");
  *out = *std::min_element(it->second.begin(), it->second.end());
  return true;
}

bool ColumnStore::Max(const std::string& column, int64_t* out) const {
  std::lock_guard<std::mutex> lock(latch);
  auto it = columns.find(column);
  if (it == columns.end()) return false;
  if (row_count == 0)
    throw std::runtime_error("ColumnStore: MAX(" + column +
                             ") over zero rows is undefined");
  *out = *std::max_element(it->second.begin(), it->second.end());
  return true;
}
}  // namespace minidb