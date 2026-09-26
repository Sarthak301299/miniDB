#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace minidb {
enum class ColumnType : uint8_t { INTEGER = 1, TEXT, VECTOR };

struct Column {
  std::string name;
  ColumnType type;
  uint32_t dim = 0;
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
  std::vector<float> vector_val;

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

  static Value Vector(std::vector<float> v) {
    Value r;
    r.type = ColumnType::VECTOR;
    r.vector_val = std::move(v);
    return r;
  }

  std::string ToString() const {
    if (type == ColumnType::INTEGER)
      return std::to_string(int_val);
    else if (type == ColumnType::TEXT)
      return text_val;
    std::string s = "[";
    for (size_t i = 0; i < vector_val.size(); ++i) {
      if (i) s += ",";
      if (i >= 4) {
        s += "...";
        break;
      }
      s += std::to_string(vector_val[i]);
    }
    s += "] (" + std::to_string(vector_val.size()) + "d)";
    return s;
  }
};
std::vector<char> SerializeRow(const Schema& schema,
                               const std::vector<Value>& values);
std::vector<Value> DeserializeRow(const Schema& schema,
                                  const std::vector<char>& bytes);

float SquaredL2Distance(const std::vector<float>& a,
                        const std::vector<float>& b);
}  // namespace minidb