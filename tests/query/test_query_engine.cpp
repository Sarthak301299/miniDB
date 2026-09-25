#include <engine.h>
#include <recovery.h>

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

int failures = 0;
void Check(bool cond, const std::string& what) {
  std::cout << (cond ? "PASS: " : "FAIL: ") << what << "\n";
  if (!cond) failures++;
}

std::string RowToString(const std::vector<Value>& row) {
  std::string s;
  for (size_t i = 0; i < row.size(); ++i) {
    if (i) s += ", ";
    s += row[i].ToString();
  }
  return s;
}

}  // namespace

int main() {
  CleanFiles();
  // === Part 1: basic CREATE/INSERT/SELECT ===
  // Testing some simple SQL queries
  std::cout << "=== Part 1: basic CREATE/INSERT/SELECT ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(16, &disk, &wal, 4);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);
    engine.Execute("CREATE TABLE users (id INT, name TEXT)", nullptr, txn_mgr);
    auto t1 = txn_mgr.Begin();
    engine.Execute("INSERT INTO users VALUES (1, 'alice')", t1.get(), txn_mgr);
    engine.Execute("INSERT INTO users VALUES (2, 'bob')", t1.get(), txn_mgr);
    engine.Execute("INSERT INTO users VALUES (3, 'charlie')", t1.get(),
                   txn_mgr);
    txn_mgr.Commit(t1.get());

    auto result = engine.Execute("SELECT * FROM users", nullptr, txn_mgr);
    std::cout << "Explain plan:\n " << result.explain << "\n";
    for (const auto& row : result.rows)
      std::cout << "row: " << RowToString(row) << "\n";
    Check(result.rows.size() == 3, "SELECT * returns all 3 committed rows");

    // === Part 2: WHERE filtering, column projection ===
    // Applying where filters rows by column values and projection selects
    // certain columns
    std::cout << "=== Part 2: WHERE filtering, column projection ===\n";
    auto r1 =
        engine.Execute("SELECT name FROM users WHERE id = 2", nullptr, txn_mgr);
    Check(r1.rows.size() == 1 && r1.rows[0].size() == 1 &&
              r1.rows[0][0].text_val == "bob",
          "WHERE id = 2 column projection returns just bob's name");
    auto r2 = engine.Execute("SELECT id FROM users WHERE id > 1 AND id < 3",
                             nullptr, txn_mgr);
    Check(r2.rows.size() == 1 && r2.rows[0].size() == 1 &&
              r2.rows[0][0].int_val == 2,
          "compound WHERE (AND) correctly narrows to a single row");

    auto r3 = engine.Execute("SELECT * FROM users WHERE name != 'bob'", nullptr,
                             txn_mgr);
    Check(
        r3.rows.size() == 2 && r3.rows[0].size() == 2 && r3.rows[1].size() == 2,
        "!= predicate correctly excludes exactly the matching row");

    // === Part 3: DELETE with MVCC ===
    // Delete respects MVCC (visible to others only after commit)
    std::cout << "=== Part 3: DELETE with MVCC ===\n";

    auto deleter = txn_mgr.Begin();
    auto del_result = engine.Execute("DELETE FROM users WHERE id = 1",
                                     deleter.get(), txn_mgr);
    Check(del_result.rows_affected == 1,
          "DELETE reports exactly one row affected");
    auto reader = engine.Execute("SELECT * FROM users", nullptr, txn_mgr);
    Check(
        reader.rows.size() == 3,
        "A concurrent reader still sees the deleted row before delete commits");
    auto readtxn = txn_mgr.Begin();
    reader = engine.Execute("SELECT * FROM users", readtxn.get(), txn_mgr);
    Check(reader.rows.size() == 3,
          "A concurrent reader as part of a transaction still sees the deleted "
          "row before delete commits");
    reader = engine.Execute("SELECT * FROM users", deleter.get(), txn_mgr);
    Check(reader.rows.size() == 2,
          "The same transaction sees the row as deleted before delete commits");
    txn_mgr.Commit(readtxn.get());
    reader = engine.Execute("SELECT * FROM users", nullptr, txn_mgr);
    Check(reader.rows.size() == 3,
          "Other transaction do not impact the commit of the delete "
          "transaction.");
    txn_mgr.Commit(deleter.get());
    auto after = engine.Execute("SELECT * FROM users", nullptr, txn_mgr);
    Check(after.rows.size() == 2,
          "After commit, a fresh reader no longer sees the deleted row");
  }
  // === Part 4: Catalog Persistence ===
  // Catalog persists across crashes
  std::cout << "=== Part 4: Catalog Persistence ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(16, &disk, &wal, 4);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    engine.Execute("CREATE TABLE orders (order_id INT, amount INT)", nullptr,
                   txn_mgr);
    auto t = txn_mgr.Begin();
    engine.Execute("INSERT INTO orders VALUES (100, 500)", t.get(), txn_mgr);
    engine.Execute("INSERT INTO orders VALUES (101, 250)", t.get(), txn_mgr);
    txn_mgr.Commit(t.get());
    std::cout << "Created 'orders', committed 2 rows.\n";
  }

  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(16, &disk, &wal, 4);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    auto result = engine.Execute("SELECT * FROM orders", nullptr, txn_mgr);
    Check(result.rows.size() == 2,
          "Catalog survived the restart. 'orders' exists without CREATE TABLE");

    bool found_100 = false, found_101 = false;
    for (const auto& row : result.rows) {
      if (row[0].int_val == 100 && row[1].int_val == 500) found_100 = true;
      if (row[0].int_val == 101 && row[1].int_val == 250) found_101 = true;
    }
    Check(found_100 && found_101,
          "Both rows' data is intact after the restart");

    bool duplicate_rejected = false;
    try {
      engine.Execute("CREATE TABLE orders (order_id INT, amount INT)", nullptr,
                     txn_mgr);
    } catch (std::exception&) {
      duplicate_rejected = true;
    }
    Check(duplicate_rejected,
          "Recreating 'orders' after restart correctly fails (table already "
          "exists)");
    auto t2 = txn_mgr.Begin();
    engine.Execute("INSERT INTO orders VALUES (102,999)", t2.get(), txn_mgr);
    txn_mgr.Commit(t2.get());
    auto after_insert =
        engine.Execute("SELECT * FROM orders", nullptr, txn_mgr);
    Check(after_insert.rows.size() == 3,
          "Can still insert into a table recovered from a prior session");
  }
  // === Part 5: Second Crash ===
  // A second crash with 2 tables
  std::cout << "=== Part 5: Second Crash ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(16, &disk, &wal, 4);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    auto result = engine.Execute("SELECT * FROM orders", nullptr, txn_mgr);
    Check(result.rows.size() == 3, "'orders' survives a second restart");
    result = engine.Execute("SELECT * FROM users", nullptr, txn_mgr);
    Check(result.rows.size() == 2, "'users' also survives a third restart");
  }

  std::cout << "\n=== Summary ===\n";
  if (failures == 0)
    std::cout << "ALL CHECKS PASSED\n";
  else
    std::cout << failures << " CHECKS FAILED\n";
  CleanFiles();
  return failures == 0 ? 0 : 1;
}