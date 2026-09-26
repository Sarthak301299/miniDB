#pragma once
#include <optional>
#include <variant>

#include "value.h"

namespace minidb {
struct Predicate {
  std::string column;
  std::string op;
  Value value;
};

struct CreateTableStmt {
  std::string table;
  Schema schema;
};

struct CreateIndexStmt {
  std::string index_name;
  std::string table;
  std::string column;
};

struct CreateVectorIndexStmt {
  std::string index_name;
  std::string table;
  std::string column;
  uint32_t num_clusters;
};

struct RefreshColumnarStmt {
  std::string table;
};

struct InsertStmt {
  std::string table;
  std::vector<Value> values;
};

struct AggregateSpec {
  std::string func;
  std::string column;
};

struct KNNSpec {
  std::string column;
  std::vector<float> query_vector;
  uint32_t k;
};

struct SelectStmt {
  std::string table;
  std::vector<std::string> columns;
  std::optional<AggregateSpec> aggregate;
  std::optional<KNNSpec> knn;
  std::vector<Predicate> where;
};

struct DeleteStmt {
  std::string table;
  std::vector<Predicate> where;
};

using Statement =
    std::variant<CreateTableStmt, CreateIndexStmt, CreateVectorIndexStmt,
                 RefreshColumnarStmt, InsertStmt, SelectStmt, DeleteStmt>;

Statement ParseStatement(const std::string& sql);

bool EvaluatePredicate(const Schema& schema, const std::vector<Value>& row,
                       const std::vector<Predicate>& predicates);
}  // namespace minidb