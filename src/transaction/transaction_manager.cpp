#include "transaction_manager.h"

namespace minidb {
TransactionManager::TransactionManager(WALManager* wal)
    : wal(wal), next_txn_id(1) {
  TxnId max = INVALID_TXN_ID;
  for (auto& r : wal->ReadAll()) {
    if (r.type == WALRecordType::COMMIT) {
      committed_ids.insert(r.txn_id);
    }
    max = std::max(max, r.txn_id);
  }
  next_txn_id.store(max + 1);
}

std::unique_ptr<Transaction> TransactionManager::Begin() {
  auto txn = std::make_unique<Transaction>();
  txn->id = next_txn_id.fetch_add(1);
  txn->state = Transaction::State::ACTIVE;
  {
    std::lock_guard<std::mutex> lock(latch);
    active_ids.insert(txn->id);
  }
  WALRecord record;
  record.type = WALRecordType::BEGIN;
  record.txn_id = txn->id;
  wal->Append(record);
  return txn;
}

void TransactionManager::Commit(Transaction* txn) {
  WALRecord record;
  record.type = WALRecordType::COMMIT;
  record.txn_id = txn->id;
  LSN lsn = wal->Append(record);
  wal->Flush(lsn);
  {
    std::lock_guard<std::mutex> lock(latch);
    committed_ids.insert(txn->id);
    active_ids.erase(txn->id);
  }
  txn->state = Transaction::State::COMMITTED;
}
void TransactionManager::Abort(Transaction* txn) {
  WALRecord record;
  record.type = WALRecordType::ABORT;
  record.txn_id = txn->id;
  LSN lsn = wal->Append(record);
  {
    std::lock_guard<std::mutex> lock(latch);
    active_ids.erase(txn->id);
  }
  txn->state = Transaction::State::ABORTED;
}
bool TransactionManager::isCommitted(TxnId txn_id) const {
  std::lock_guard<std::mutex> lock(latch);
  return committed_ids.contains(txn_id);
}
bool TransactionManager::isVisible(const TupleHeader& header,
                                   TxnId txn_id) const {
  if (header.xmin != txn_id && !isCommitted(header.xmin)) return false;
  if (header.xmax != INVALID_TXN_ID) {
    if (header.xmax == txn_id) return false;
    if (isCommitted(header.xmax)) return false;
  }
  return true;
}
}  // namespace minidb