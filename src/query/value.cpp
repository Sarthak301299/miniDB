#include "value.h"

#include <cstring>
#include <stdexcept>

namespace minidb {

namespace {
std::vector<size_t> CodePointStarts(const std::string& s) {
  std::vector<size_t> starts;
  size_t i = 0;
  while (i < s.size()) {
    starts.push_back(i);
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80           ? 1
                 : (c >> 5) == 0x6  ? 2
                 : (c >> 4) == 0xE  ? 3
                 : (c >> 3) == 0x1E ? 4
                                    : 1;
    size_t j = 1;
    while (j < len && i + j < s.size() &&
           (static_cast<unsigned char>(s[i + j]) & 0xC0) == 0x80) {
      ++j;
    }
    i += j;
  }
  return starts;
}
}  // namespace

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
    } else if (schema.columns[i].type == ColumnType::TEXT) {
      uint32_t str_len = static_cast<uint32_t>(values[i].text_val.size());
      const char* start = reinterpret_cast<const char*>(&str_len);
      const char* end = start + sizeof(str_len);
      out.insert(out.end(), start, end);
      out.insert(out.end(), values[i].text_val.begin(),
                 values[i].text_val.end());
    } else {
      if (values[i].vector_val.size() != schema.columns[i].dim)
        throw std::runtime_error(
            "SerializeRow: column " + schema.columns[i].name +
            " expects dimension " + std::to_string(schema.columns[i].dim) +
            ", got " + std::to_string(values[i].vector_val.size()));
      const char* p =
          reinterpret_cast<const char*>(values[i].vector_val.data());
      out.insert(out.end(), p, p + sizeof(float) * schema.columns[i].dim);
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
    } else if (column.type == ColumnType::TEXT) {
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
    } else {
      size_t nbytes = sizeof(float) * static_cast<size_t>(column.dim);
      if (offset + nbytes > bytes.size())
        throw std::runtime_error(
            "DeserializeRow: buffer too short for vector of length " +
            std::to_string(column.dim) + " of column " + column.name);

      std::vector<float> extract(column.dim);
      std::memcpy(extract.data(), bytes.data() + offset, nbytes);
      offset += nbytes;
      Value v = Value::Vector(std::move(extract));
      out.push_back(v);
    }
  }
  return out;
}

Value MaskValue(const Column& col, const Value& v) {
  if (col.type == ColumnType::VECTOR) return Value::Text("[MASKED]");
  std::string s =
      col.type == ColumnType::INTEGER ? std::to_string(v.int_val) : v.text_val;
  auto starts = CodePointStarts(s);
  const size_t n = starts.size();
  const size_t keep =
      static_cast<size_t>(col.mask_first) + static_cast<size_t>(col.mask_last);
  const bool all = n <= keep;
  const size_t head = all ? 0 : col.mask_first;
  const size_t tail = all ? 0 : col.mask_last;
  std::string out;
  if (head) out.append(s, 0, starts[head]);
  out.append(n - head - tail, 'X');
  if (tail) out.append(s, starts[n - tail], std::string::npos);
  return Value::Text(std::move(out));
}

float SquaredL2Distance(const std::vector<float>& a,
                        const std::vector<float>& b) {
  if (a.size() != b.size())
    throw std::runtime_error(
        "SquaredL2Distance: The input vectors have different sizes" +
        std::to_string(a.size()) + " vs. " + std::to_string(b.size()));
  float out = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    float d = (a[i] - b[i]);
    out += d * d;
  }
  return out;
}

}  // namespace minidb