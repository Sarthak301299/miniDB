#pragma once
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

struct InsertStmt {
  std::string table;
  std::vector<Value> values;
};

struct SelectStmt {
  std::string table;
  std::vector<std::string> columns;
  std::vector<Predicate> where;
};

struct DeleteStmt {
  std::string table;
  std::vector<Predicate> where;
};

using Statement =
    std::variant<CreateTableStmt, InsertStmt, SelectStmt, DeleteStmt>;

Statement ParseStatement(const std::string& sql);

bool EvaluatePredicate(const Schema& schema, const std::vector<Value>& row,
                       const std::vector<Predicate>& predicates);
}  // namespace minidb