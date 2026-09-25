#include "value.h"

#include <cstring>
#include <stdexcept>

namespace minidb {
std::vector<char> SerializeRow(const Schema& schema,
                               const std::vector<Value>& values) {
  if (schema.columns.size() != values.size())
    throw std::runtime_error(
        "SerializeRow: Mismatch between "
        "column sizes of schema: " +
        std::to_string(schema.columns.size()) +
        " and values: " + std::to_string(values.size()));

  std::vector<char> out;

  for (size_t i = 0; i < values.size(); ++i) {
    if (schema.columns[i].type != values[i].type)
      throw std::runtime_error(
          "SerializeRow: Mismatch between column types of schema and values at "
          "column " +
          i);
    if (schema.columns[i].type == ColumnType::INTEGER) {
      const char* start = reinterpret_cast<const char*>(&values[i].int_val);
      const char* end = start + sizeof(values[i].int_val);
      out.insert(out.end(), start, end);
    } else {
      uint32_t str_len = static_cast<uint32_t>(values[i].text_val.size());
      const char* start = reinterpret_cast<const char*>(&str_len);
      const char* end = start + sizeof(str_len);
      out.insert(out.end(), start, end);
      out.insert(out.end(), values[i].text_val.begin(),
                 values[i].text_val.end());
    }
  }
  return out;
}
std::vector<Value> DeserializeRow(const Schema& schema,
                                  const std::vector<char>& bytes) {
  std::vector<Value> out;
  size_t offset = 0;
  for (const auto& column : schema.columns) {
    if (column.type == ColumnType::INTEGER) {
      if (offset + sizeof(int64_t) > bytes.size())
        throw std::runtime_error(
            "DeserializeRow: buffer too short for column name " + column.name);
      int64_t extract;
      std::memcpy(&extract, bytes.data() + offset, sizeof(extract));
      offset += sizeof(extract);
      Value v = Value::Int(extract);
      out.push_back(v);
    } else {
      if (offset + sizeof(uint32_t) > bytes.size())
        throw std::runtime_error(
            "DeserializeRow: buffer too short for text length of column " +
            column.name);
      uint32_t str_len;
      std::memcpy(&str_len, bytes.data() + offset, sizeof(str_len));
      offset += sizeof(str_len);
      if (offset + static_cast<size_t>(str_len) > bytes.size())
        throw std::runtime_error(
            "DeserializeRow: buffer too short for text of column " +
            column.name);
      std::string extract(bytes.begin() + static_cast<long>(offset),
                          bytes.begin() + static_cast<long>(offset + str_len));
      offset += static_cast<size_t>(str_len);
      Value v = Value::Text(extract);
      out.push_back(v);
    }
  }
  return out;
}
}  // namespace minidb