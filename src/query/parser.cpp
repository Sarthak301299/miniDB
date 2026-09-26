#include "parser.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace minidb {

namespace {
enum class TokenType { IDENT, NUMBER, STRING, SYMBOL, END };
struct Token {
  TokenType type;
  std::string text;
};

std::string Upper(const std::string& s) {
  std::string r = s;
  for (auto& c : r)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

class Lexer {
 private:
  const std::string& s;
  size_t pos = 0;
  void SkipWhitespace() {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
      pos++;
  }

  Token ReadIdent() {
    size_t start = pos;
    while (pos < s.size() &&
           (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '_'))
      pos++;
    return {TokenType::IDENT, s.substr(start, pos - start)};
  }

  Token ReadNumber() {
    size_t start = pos;
    if (s[pos] == '-') pos++;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
      pos++;
    if (pos < s.size() && s[pos] == '.') {
      pos++;
      while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
        pos++;
    }
    return {TokenType::NUMBER, s.substr(start, pos - start)};
  }

  Token ReadString() {
    pos++;
    size_t start = pos;
    while (pos < s.size() && s[pos] != '\'') pos++;
    if (pos >= s.size())
      throw std::runtime_error("ReadString: Unterminated string literal");
    std::string text = s.substr(start, pos - start);
    pos++;
    return {TokenType::STRING, text};
  }

  Token ReadOperator() {
    size_t start = pos;
    if (s[pos] == '<' && pos + 2 < s.size() && s[pos + 1] == '-' &&
        s[pos + 2] == '>') {
      pos += 3;
      return {TokenType::SYMBOL, "<->"};
    }
    pos++;
    if (pos < s.size() && s[pos] == '=') pos++;
    return {TokenType::SYMBOL, s.substr(start, pos - start)};
  }

 public:
  explicit Lexer(const std::string& s) : s(s) {}
  std::vector<Token> Tokenize() {
    std::vector<Token> tokens;
    while (true) {
      SkipWhitespace();
      if (pos >= s.size()) {
        tokens.push_back({TokenType::END, ""});
        break;
      }
      char c = s[pos];
      if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        tokens.push_back(ReadIdent());
      } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                 (c == '-' && pos + 1 < s.size() &&
                  std::isdigit(static_cast<unsigned char>(s[pos + 1])))) {
        tokens.push_back(ReadNumber());
      } else if (c == '\'') {
        tokens.push_back(ReadString());
      } else if (c == '<' || c == '>' || c == '!' || c == '=') {
        tokens.push_back(ReadOperator());
      } else if (std::string("(),;*[]").find(c) != std::string::npos) {
        pos++;
        tokens.push_back({TokenType::SYMBOL, std::string(1, c)});
      } else {
        throw std::runtime_error("Tokenize: Unexpected character " +
                                 std::to_string(c) + " in SQL");
      }
    }
    return tokens;
  }
};

class Parser {
 private:
  std::vector<Token> tokens;
  size_t pos;
  const Token& Peek() const { return tokens[pos]; }
  const Token& Advance() { return tokens[pos++]; }
  const Token& PeekNext() const {
    return tokens[pos + 1 < tokens.size() ? pos + 1 : tokens.size() - 1];
  }

  void ExpectKeyword(const std::string& kw) {
    if (Upper(Peek().text) != kw) {
      throw std::runtime_error("ExpectKeyword: Expected " + kw + " got " +
                               Peek().text);
    }
    Advance();
  }

  void ExpectSymbol(const std::string& sym) {
    if (Peek().type != TokenType::SYMBOL || Peek().text != sym) {
      throw std::runtime_error("ExpectSymbol: Expected " + sym + " got " +
                               Peek().text);
    }
    Advance();
  }

  std::string ExpectIdent() {
    if (Peek().type != TokenType::IDENT)
      throw std::runtime_error("ExpectIdent: Expected identifier got " +
                               Peek().text);
    return Advance().text;
  }

  bool AtSymbol(const std::string& sym) const {
    return Peek().type == TokenType::SYMBOL && Peek().text == sym;
  }

  bool AtKeyword(const std::string& kw) const {
    return Peek().type == TokenType::IDENT && Upper(Peek().text) == kw;
  }

  std::vector<float> ParseVectorLiteral() {
    ExpectSymbol("[");
    std::vector<float> v;
    if (!AtSymbol("]")) {
      while (true) {
        if (Peek().type != TokenType::NUMBER)
          throw std::runtime_error("Expected a number is vector literal, got" +
                                   Peek().text);
        v.push_back(std::stof(Advance().text));
        if (AtSymbol(",")) {
          Advance();
          continue;
        }
        break;
      }
    }
    ExpectSymbol("]");
    return v;
  }

  Value ExpectLiteral() {
    if (AtSymbol("[")) return Value::Vector(ParseVectorLiteral());
    if (Peek().type == TokenType::NUMBER)
      return Value::Int(std::stoll(Advance().text));
    if (Peek().type == TokenType::STRING) return Value::Text(Advance().text);
    throw std::runtime_error("ExpectLiteral: Expected a literal value, got " +
                             Peek().text);
  }

  CreateTableStmt ParseCreateTable() {
    ExpectKeyword("TABLE");
    CreateTableStmt stmt;
    stmt.table = ExpectIdent();
    ExpectSymbol("(");
    while (true) {
      Column col;
      col.name = ExpectIdent();
      std::string type_kw = Upper(ExpectIdent());
      if (type_kw == "INT" || type_kw == "INTEGER") {
        col.type = ColumnType::INTEGER;
      } else if (type_kw == "TEXT" || type_kw == "VARCHAR") {
        col.type = ColumnType::TEXT;
      } else if (type_kw == "VECTOR") {
        col.type = ColumnType::VECTOR;
        ExpectSymbol("(");
        if (Peek().type != TokenType::NUMBER)
          throw std::runtime_error(
              "Expected a dimension number in VECTOR(dim), got " + Peek().text);
        col.dim = static_cast<uint32_t>(std::stoul(Advance().text));
        ExpectSymbol(")");
      } else
        throw std::runtime_error("ParseCreateTable: Unknown column type " +
                                 type_kw + " (supported: INT, TEXT)");

      stmt.schema.name_to_index[col.name] =
          static_cast<uint64_t>(stmt.schema.columns.size());
      stmt.schema.columns.push_back(col);
      if (AtSymbol(",")) {
        Advance();
        continue;
      }
      break;
    }
    ExpectSymbol(")");
    if (AtSymbol(";")) Advance();
    return stmt;
  }

  CreateIndexStmt ParseCreateIndex() {
    ExpectKeyword("INDEX");
    CreateIndexStmt stmt;
    stmt.index_name = ExpectIdent();
    ExpectKeyword("ON");
    stmt.table = ExpectIdent();
    ExpectSymbol("(");
    stmt.column = ExpectIdent();
    ExpectSymbol(")");
    if (AtSymbol(";")) Advance();
    return stmt;
  }

  CreateVectorIndexStmt ParseCreateVectorIndex() {
    ExpectKeyword("VECTOR");
    ExpectKeyword("INDEX");
    CreateVectorIndexStmt stmt;
    stmt.index_name = ExpectIdent();
    ExpectKeyword("ON");
    stmt.table = ExpectIdent();
    ExpectSymbol("(");
    stmt.column = ExpectIdent();
    ExpectSymbol(")");
    ExpectKeyword("LISTS");
    if (Peek().type != TokenType::NUMBER)
      throw std::runtime_error("Expected a number after LISTS, got " +
                               Peek().text);
    stmt.num_clusters = static_cast<uint32_t>(std::stoul(Advance().text));
    if (AtSymbol(";")) Advance();
    return stmt;
  }

  Statement ParseCreate() {
    ExpectKeyword("CREATE");
    if (AtKeyword("TABLE")) return ParseCreateTable();
    if (AtKeyword("INDEX")) return ParseCreateIndex();
    if (AtKeyword("VECTOR")) return ParseCreateVectorIndex();
    throw std::runtime_error(
        "ParseCreate: Expected TABLE or INDEX after CREATE, got " +
        Peek().text);
  }

  RefreshColumnarStmt ParseRefreshColumnar() {
    ExpectKeyword("REFRESH");
    ExpectKeyword("COLUMNAR");
    RefreshColumnarStmt stmt;
    stmt.table = ExpectIdent();
    if (AtSymbol(";")) Advance();
    return stmt;
  }

  InsertStmt ParseInsert() {
    ExpectKeyword("INSERT");
    ExpectKeyword("INTO");
    InsertStmt stmt;
    stmt.table = ExpectIdent();
    ExpectKeyword("VALUES");
    ExpectSymbol("(");
    while (true) {
      stmt.values.push_back(ExpectLiteral());
      if (AtSymbol(",")) {
        Advance();
        continue;
      }
      break;
    }
    ExpectSymbol(")");
    if (AtSymbol(";")) Advance();
    return stmt;
  }

  std::vector<Predicate> ParseWhereClause() {
    static const std::vector<std::string> ops = {
        "=", "!=", "<=", ">=", "<", ">"};
    std::vector<Predicate> preds;
    while (true) {
      Predicate p;
      p.column = ExpectIdent();
      if (Peek().type != TokenType::SYMBOL ||
          std::find(ops.begin(), ops.end(), Peek().text) == ops.end())
        throw std::runtime_error(
            "ParseWhereClause: Expected a comparison operator, got " +
            Peek().text);
      p.op = Advance().text;
      p.value = ExpectLiteral();
      preds.push_back(std::move(p));
      if (AtKeyword("AND")) {
        Advance();
        continue;
      }
      break;
    }
    return preds;
  }

  SelectStmt ParseSelect() {
    ExpectKeyword("SELECT");
    SelectStmt stmt;
    static const std::vector<std::string> aggFuncs = {"SUM", "AVG", "MIN",
                                                      "MAX", "COUNT"};
    bool is_agg = Peek().type == TokenType::IDENT &&
                  std::find(aggFuncs.begin(), aggFuncs.end(),
                            Upper(Peek().text)) != aggFuncs.end() &&
                  PeekNext().type == TokenType::SYMBOL &&
                  PeekNext().text == "(";
    if (is_agg) {
      AggregateSpec agg;
      agg.func = Upper(Advance().text);
      ExpectSymbol("(");
      if (AtSymbol("*")) {
        if (agg.func != "COUNT")
          throw std::runtime_error(agg.func +
                                   "(*) is not supported. Only COUNT(*) is");
        Advance();
        agg.column = "*";
      } else {
        agg.column = ExpectIdent();
      }
      ExpectSymbol(")");
      stmt.aggregate = agg;
    } else if (AtSymbol("*")) {
      Advance();
    } else {
      while (true) {
        stmt.columns.push_back(ExpectIdent());
        if (AtSymbol(",")) {
          Advance();
          continue;
        }
        break;
      }
    }
    ExpectKeyword("FROM");
    stmt.table = ExpectIdent();
    if (AtKeyword("WHERE")) {
      Advance();
      stmt.where = ParseWhereClause();
    }
    if (AtKeyword("ORDER")) {
      if (stmt.aggregate)
        throw std::runtime_error(
            "ORDER BY ... <-> ... is not supported with and aggregate "
            "function");

      Advance();
      ExpectKeyword("BY");
      KNNSpec knn;
      knn.column = ExpectIdent();
      ExpectSymbol("<->");
      knn.query_vector = ParseVectorLiteral();
      ExpectKeyword("LIMIT");
      if (Peek().type != TokenType::NUMBER)
        throw std::runtime_error("Expected a number after LIMIT, got " +
                                 Peek().text);
      knn.k = static_cast<uint32_t>(std::stoul(Advance().text));
      stmt.knn = knn;
    }
    if (AtSymbol(";")) Advance();
    return stmt;
  }

  DeleteStmt ParseDelete() {
    ExpectKeyword("DELETE");
    ExpectKeyword("FROM");
    DeleteStmt stmt;
    stmt.table = ExpectIdent();
    if (AtKeyword("WHERE")) {
      Advance();
      stmt.where = ParseWhereClause();
    }
    if (AtSymbol(";")) Advance();
    return stmt;
  }

 public:
  explicit Parser(std::vector<Token> tokens)
      : tokens(std::move(tokens)), pos(0) {}
  Statement Parse() {
    std::string kw = Upper(Peek().text);
    if (kw == "CREATE") return ParseCreate();
    if (kw == "REFRESH") return ParseRefreshColumnar();
    if (kw == "INSERT") return ParseInsert();
    if (kw == "SELECT") return ParseSelect();
    if (kw == "DELETE") return ParseDelete();
    throw std::runtime_error(
        "Parse: Expected CREATE, REFRESH, INSERT, SELECT, or DELETE, got " +
        Peek().text);
  }
};
}  // namespace
Statement ParseStatement(const std::string& sql) {
  Lexer lexer(sql);
  Parser parser(lexer.Tokenize());
  return parser.Parse();
}

bool EvaluatePredicate(const Schema& schema, const std::vector<Value>& row,
                       const std::vector<Predicate>& predicates) {
  for (auto& pred : predicates) {
    int idx = schema.ColumnIndex(pred.column);
    if (idx < 0)
      throw std::runtime_error("EvaluatePredicate: Unknwon column " +
                               pred.column + "in WHERE clause");
    const Value& v = row[static_cast<size_t>(idx)];
    if (v.type != pred.value.type)
      throw std::runtime_error(
          "EvaluatePredicate: Type mismatch comparing column " + pred.column);
    int cmp;
    if (v.type == ColumnType::INTEGER) {
      cmp = (v.int_val < pred.value.int_val
                 ? -1
                 : (v.int_val > pred.value.int_val ? 1 : 0));
    } else {
      int str_cmp = v.text_val.compare(pred.value.text_val);
      cmp = (str_cmp < 0) ? -1 : (str_cmp > 0 ? 1 : 0);
    }
    bool ok;
    if (pred.op == "=")
      ok = (cmp == 0);
    else if (pred.op == "!=")
      ok = (cmp != 0);
    else if (pred.op == "<")
      ok = (cmp < 0);
    else if (pred.op == "<=")
      ok = (cmp <= 0);
    else if (pred.op == ">")
      ok = (cmp > 0);
    else if (pred.op == ">=")
      ok = (cmp >= 0);
    else
      throw std::runtime_error("EvaluatePredicate: Unsupported operator " +
                               pred.op);
    if (!ok) return false;
  }
  return true;
}
}  // namespace minidb