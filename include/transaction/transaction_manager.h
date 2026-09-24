#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_set>

#include "transaction.h"
#include "tuple.h"
#include "wal_manager.h"

namespace minidb {
class TransactionManager {
 private:
  WALManager* wal;
  std::atomic<TxnId> next_txn_id = 1;
  mutable std::mutex latch;
  std::unordered_set<TxnId> committed_ids;
  std::unordered_set<TxnId> active_ids;

 public:
  explicit TransactionManager(WALManager* wal);
  std::unique_ptr<Transaction> Begin();
  void Commit(Transaction* txn);
  void Abort(Transaction* txn);
  bool isCommitted(TxnId txn_id) const;
  bool isVisible(const TupleHeader& header, TxnId txn_id) const;
};
}  // namespace minidb