#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace minidb {
enum class ColumnType : uint8_t { INTEGER = 1, TEXT };

struct Column {
  std::string name;
  ColumnType type;
};

struct Schema {
  std::vector<Column> columns;
  std::unordered_map<std::string, uint64_t> name_to_index;
  int ColumnIndex(const std::string& name) const {
    auto it = name_to_index.find(name);
    if (it != name_to_index.end()) return static_cast<uint64_t>(it->second);
    return -1;
  }
};

struct Value {
  ColumnType type = ColumnType::INTEGER;
  int64_t int_val = 0;
  std::string text_val;

  static Value Int(int64_t v) {
    Value r;
    r.type = ColumnType::INTEGER;
    r.int_val = v;
    return r;
  }

  static Value Text(std::string v) {
    Value r;
    r.type = ColumnType::TEXT;
    r.text_val = std::move(v);
    return r;
  }

  std::string ToString() const {
    return type == ColumnType::INTEGER ? std::to_string(int_val) : text_val;
  }
};
std::vector<char> SerializeRow(const Schema& schema,
                               const std::vector<Value>& values);
std::vector<Value> DeserializeRow(const Schema& schema,
                                  const std::vector<char>& bytes);
}  // namespace minidb