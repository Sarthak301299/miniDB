#pragma once

#include <memory>
#include <vector>

#include "heap_file.h"
#include "parser.h"
#include "value.h"
namespace minidb {

class Executor {
 public:
  virtual ~Executor() = default;
  virtual void Open() = 0;
  virtual bool Next(std::vector<Value>* out_row) = 0;
  virtual void Close() = 0;
};

class SeqScanExecutor : public Executor {
 private:
  HeapFile* heap;
  const Schema* schema;
  TxnId reader_txn;
  const TransactionManager* txn_mgr;
  std::vector<std::pair<RID, std::vector<char>>> rows;
  size_t pos = 0;

 public:
  SeqScanExecutor(HeapFile* heap, const Schema* schema, TxnId reader_txn,
                  const TransactionManager* txn_mgr);
  void Open() override;
  bool Next(std::vector<Value>* out_row) override;
  void Close() override;
};

class FilterExecutor : public Executor {
 private:
  std::unique_ptr<Executor> child;
  const Schema* schema;
  std::vector<Predicate> predicates;

 public:
  FilterExecutor(std::unique_ptr<Executor> child, const Schema* schema,
                 std::vector<Predicate> predicates);
  void Open() override;
  bool Next(std::vector<Value>* out_row) override;
  void Close() override;
};

class ProjectionExecutor : public Executor {
 private:
  std::unique_ptr<Executor> child;
  std::vector<int> colidx;

 public:
  ProjectionExecutor(std::unique_ptr<Executor> child, const Schema* schema,
                     std::vector<std::string> columns);
  void Open() override;
  bool Next(std::vector<Value>* out_row) override;
  void Close() override;
};
}  // namespace minidb