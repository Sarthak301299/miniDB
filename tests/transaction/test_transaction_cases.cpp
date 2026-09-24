#include <buffer_pool.h>
#include <disk_manager.h>
#include <heap_file.h>
#include <recovery.h>
#include <transaction_manager.h>
#include <wal_manager.h>

#include <iostream>

using namespace minidb;

namespace {
constexpr const char* db_file = "mini.db";
constexpr const char* log_file = "mini.wal";
void CleanFiles() {
  std::remove(db_file);
  std::remove(log_file);
}

std::vector<char> Bytes(const std::string& str) {
  return std::vector<char>(str.begin(), str.end());
}

std::string Str(std::vector<char>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

void RunRecovery(DiskManager& disk, WALManager& wal) {
  RecoveryManager recovery(&disk, &wal);
  size_t redone = recovery.Recover();
  if (redone > 0)
    std::cout << "Recovery completed. Redone " << redone << " page updates.\n";
}

int failures = 0;
void Check(bool cond, const std::string& what) {
  std::cout << (cond ? "PASS: " : "FAIL: ") << what << "\n";
  if (!cond) failures++;
}

}  // namespace

int main() {
  CleanFiles();
  // === Part 1: Commit Durability ===
  // A committed insert survives a crash
  std::cout << "=== Part 1: Commit Durability ===\n";
  RID committed_rid;
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RunRecovery(disk, wal);
    BufferPool pool(16, &disk, &wal, 4);
    TransactionManager txn_manager(&wal);
    HeapFile heap(&pool, &wal);

    auto txn = txn_manager.Begin();
    committed_rid = heap.InsertTuple(txn.get(), Bytes("Part 1 Committed"));
    txn_manager.Commit(txn.get());
    std::cout << "Inserted and committed row at page " << committed_rid.page_id
              << " slot " << committed_rid.slot << "\n";
  }  // Simulated crash: Nothing is explicitly checkpointed/flushed.

  // === Part 2: MVCC visibility ===
  // Uncommitted data is invisible after a crash even if the bytes are written
  // to disk.
  std::cout << "=== Part 2: MVCC visibility ===\n";
  RID uncommitted_rid;
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RunRecovery(disk, wal);
    BufferPool pool(16, &disk, &wal, 4);
    TransactionManager txn_manager(&wal);
    HeapFile heap(&pool, &wal);

    auto txn = txn_manager.Begin();
    uncommitted_rid =
        heap.InsertTuple(txn.get(), Bytes("Part 2 Insert without Commit"));
    std::cout << "Inserted (not committed) row at page "
              << uncommitted_rid.page_id << " slot " << uncommitted_rid.slot
              << "\n";
  }  // Simulated crash: Nothing is explicitly checkpointed/flushed.

  // Checking correctness for Part 1 and Part 2
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RunRecovery(disk, wal);
    BufferPool pool(16, &disk, &wal, 4);
    TransactionManager txn_manager(&wal);
    HeapFile heap(&pool, &wal);

    Check(committed_rid.page_id != uncommitted_rid.page_id ||
              committed_rid.slot != uncommitted_rid.slot,
          "Part 1 and Part 2 landed on different rows (page allocation did not "
          "collide)");
    TupleHeader header;
    std::vector<char> data;
    bool found = heap.ReadTuple(uncommitted_rid, &header, &data);
    Check(found && Str(data) == "Part 2 Insert without Commit",
          "The Uncommitted row's bytes were recovered correctly.");
    bool visible = txn_manager.isVisible(header, INVALID_TXN_ID);
    Check(
        !visible,
        "The Uncommitted row is not visible to a fresh reader after recovery.");

    found = heap.ReadTuple(committed_rid, &header, &data);
    visible = found && txn_manager.isVisible(header, INVALID_TXN_ID);
    Check(visible && Str(data) == "Part 1 Committed",
          "Committed row from Part 1 is still visible and correct after "
          "recovery");
  }

  // === Part 3: Explicit Abort ===
  // A live abort hides data immediately.
  std::cout << "=== Part 3: Explicit Abort ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RunRecovery(disk, wal);
    BufferPool pool(16, &disk, &wal, 4);
    TransactionManager txn_manager(&wal);
    HeapFile heap(&pool, &wal);

    auto txn = txn_manager.Begin();
    RID rid = heap.InsertTuple(txn.get(), Bytes("Part 3 Aborting"));

    TupleHeader header;
    std::vector<char> data;
    heap.ReadTuple(rid, &header, &data);

    Check(txn_manager.isVisible(header, txn->id),
          "An Active transaction can see its own uncommitted insert");

    txn_manager.Abort(txn.get());
    heap.ReadTuple(rid, &header, &data);

    Check(Str(data) == "Part 3 Aborting",
          "The aborted transaction's row still remain on the page");
    Check(!txn_manager.isVisible(header, INVALID_TXN_ID),
          "The aborted transaction's rows are invisible to a fresh reader "
          "without any crash");
  }

  // === Phase 4: Delete Lifecycle ===
  std::cout << "=== Phase 4: Delete Lifecycle ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RunRecovery(disk, wal);
    BufferPool pool(16, &disk, &wal, 4);
    TransactionManager txn_manager(&wal);
    HeapFile heap(&pool, &wal);

    auto writer_txn = txn_manager.Begin();
    RID rid =
        heap.InsertTuple(writer_txn.get(), Bytes("Part 4 data to be deleted"));

    txn_manager.Commit(writer_txn.get());

    auto delete_txn = txn_manager.Begin();
    Check(heap.DeleteTuple(delete_txn.get(), rid),
          "Delete on an existing row succeeds");

    TupleHeader header;
    std::vector<char> data;
    heap.ReadTuple(rid, &header, &data);

    Check(txn_manager.isVisible(header, INVALID_TXN_ID),
          "The deleted row is still visible to a concurrent reader while the "
          "delete is not committed");
    Check(!txn_manager.isVisible(header, delete_txn->id),
          "The deleted row is invisible to the deleter itself at the point of "
          "invoking delete instead of commit");

    txn_manager.Commit(delete_txn.get());
    heap.ReadTuple(rid, &header, &data);
    Check(!txn_manager.isVisible(header, INVALID_TXN_ID),
          "The deleted row is no longer visible to a concurrent reader after "
          "the delete is committed");

    writer_txn = txn_manager.Begin();
    rid = heap.InsertTuple(writer_txn.get(),
                           Bytes("Part 4 delete to be aborted"));
    txn_manager.Commit(writer_txn.get());
    delete_txn = txn_manager.Begin();
    heap.DeleteTuple(delete_txn.get(), rid);
    txn_manager.Abort(delete_txn.get());
    heap.ReadTuple(rid, &header, &data);
    Check(txn_manager.isVisible(header, INVALID_TXN_ID),
          "Aborting the deleted transaction leaves the row visible without "
          "undoing xmax");
  }

  // === Part 5: Read Visibility ===
  // Visibility is checked live and not snapshotted at Begin()
  std::cout << "=== Part 5: Read visibility ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RunRecovery(disk, wal);
    BufferPool pool(16, &disk, &wal, 4);
    TransactionManager txn_manager(&wal);
    HeapFile heap(&pool, &wal);

    auto writer_txn = txn_manager.Begin();
    RID rid =
        heap.InsertTuple(writer_txn.get(), Bytes("Part 5 visibility checking"));

    auto reader_txn = txn_manager.Begin();
    TupleHeader header;
    std::vector<char> data;
    heap.ReadTuple(rid, &header, &data);
    Check(!txn_manager.isVisible(header, reader_txn->id),
          "Reader does not see writer's row before it is committed.");
    txn_manager.Commit(writer_txn.get());
    Check(txn_manager.isVisible(header, reader_txn->id),
          "The same reader sees writer's row after it is committed.");
    txn_manager.Commit(reader_txn.get());
  }

  std::cout << "\n=== Summary ===\n";
  if (failures == 0)
    std::cout << "ALL CHECKS PASSED\n";
  else
    std::cout << failures << " CHECKS FAILED\n";

  CleanFiles();
  return failures == 0 ? 0 : 1;
}