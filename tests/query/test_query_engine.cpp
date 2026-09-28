#include <engine.h>
#include <recovery.h>

#include <chrono>
#include <iostream>
#include <random>
#include <set>
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

std::vector<float> RandVec(std::mt19937& rng, int dim) {
  std::uniform_real_distribution<float> dist(0.0f, 100.0f);
  std::vector<float> v(dim);
  for (auto& x : v) x = dist(rng);
  return v;
}

std::string VecLiteral(const std::vector<float>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += std::to_string(v[i]);
  }
  s += "]";
  return s;
}

std::vector<int64_t> BruteForceKNN(
    const std::vector<std::pair<std::vector<float>, int64_t>>& data,
    const std::vector<float>& query, uint32_t k) {
  std::vector<std::pair<float, int64_t>> dists;
  for (auto& [v, id] : data) dists.push_back({SquaredL2Distance(query, v), id});
  std::sort(dists.begin(), dists.end(),
            [](auto& a, auto& b) { return a.first < b.first; });
  std::vector<int64_t> result;
  for (size_t i = 0; i < dists.size() && i < k; ++i)
    result.push_back(dists[i].second);
  return result;
}

double RecallAtK(const std::vector<int64_t>& approx,
                 const std::vector<int64_t>& exact) {
  std::set<int64_t> exact_set(exact.begin(), exact.end());
  int hits = 0;
  for (auto id : approx) {
    if (exact_set.count(id)) hits++;
  }
  return exact.empty()
             ? 1.0
             : static_cast<double>(hits) / static_cast<double>(exact.size());
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

  CleanFiles();
  // === Part 6: Create Index ===
  // Create index on an existing, populated table
  std::cout << "=== Part 6: Create Index ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(64, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    engine.Execute("CREATE TABLE accounts (id INT, balance INT)", nullptr,
                   txn_mgr);

    auto t1 = txn_mgr.Begin();
    for (int i = 0; i < 50; ++i) {
      std::string sql = "INSERT INTO accounts VALUES (" + std::to_string(i) +
                        ", " + std::to_string(i * 100) + ")";
      engine.Execute(sql, t1.get(), txn_mgr);
    }
    txn_mgr.Commit(t1.get());

    auto before = engine.Execute("SELECT * FROM accounts WHERE id = 25",
                                 nullptr, txn_mgr);
    std::cout << "Before index: " << before.explain << "\n";
    Check(before.explain.find("SeqScan") != std::string::npos,
          "before CREATE INDEX, WHERE id = 25 uses SeqScan");
    Check(before.rows.size() == 1 && before.rows[0][1].int_val == 2500,
          "SeqScan still finds the correct row (id=25, balance=2500)");

    engine.Execute("CREATE INDEX idx_id ON accounts (id)", nullptr, txn_mgr);

    auto after = engine.Execute("SELECT * FROM accounts WHERE id = 25", nullptr,
                                txn_mgr);
    std::cout << "After index: " << after.explain << "\n";
    Check(after.explain.find("IndexScan") != std::string::npos,
          "after CREATE INDEX, the SAME query now uses IndexScan");
    Check(
        after.rows.size() == 1 && after.rows[0][1].int_val == 2500,
        "IndexScan finds the exact same correct row via the bulk-loaded index");

    auto other_col = engine.Execute(
        "SELECT * FROM accounts WHERE balance = 2500", nullptr, txn_mgr);
    Check(other_col.explain.find("SeqScan") != std::string::npos,
          "WHERE on the NON-indexed column still uses SeqScan");

    auto combined = engine.Execute(
        "SELECT * FROM accounts WHERE id = 25 AND balance = 2500", nullptr,
        txn_mgr);
    std::cout << "Combined predicate plan: " << combined.explain << "\n";
    Check(combined.explain.find("IndexScan") != std::string::npos &&
              combined.explain.find("Filter") != std::string::npos,
          "combined predicate (indexed + non-indexed) uses IndexScan wrapped "
          "in a Filter");
    Check(combined.rows.size() == 1,
          "combined predicate still returns exactly the right row");
  }

  std::cout
      << "\n=== Part 7: index maintenance -- new INSERTs stay in sync ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(64, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    auto t = txn_mgr.Begin();
    engine.Execute("INSERT INTO accounts VALUES (999, 77777)", t.get(),
                   txn_mgr);
    txn_mgr.Commit(t.get());

    auto result = engine.Execute("SELECT * FROM accounts WHERE id = 999",
                                 nullptr, txn_mgr);
    Check(result.explain.find("IndexScan") != std::string::npos,
          "query for a row inserted AFTER CREATE INDEX still uses IndexScan");
    Check(result.rows.size() == 1 && result.rows[0][1].int_val == 77777,
          "the newly inserted row is found correctly via the index "
          "(maintenance works)");
  }

  std::cout
      << "\n=== Part 8: delete + reinsert of the same indexed value ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(64, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    auto d = txn_mgr.Begin();
    engine.Execute("DELETE FROM accounts WHERE id = 999", d.get(), txn_mgr);
    txn_mgr.Commit(d.get());

    auto gone = engine.Execute("SELECT * FROM accounts WHERE id = 999", nullptr,
                               txn_mgr);
    Check(gone.rows.empty(),
          "after commit, the deleted row is invisible via IndexScan too");

    auto i2 = txn_mgr.Begin();
    engine.Execute("INSERT INTO accounts VALUES (999, 11111)", i2.get(),
                   txn_mgr);
    txn_mgr.Commit(i2.get());

    auto reinserted = engine.Execute("SELECT * FROM accounts WHERE id = 999",
                                     nullptr, txn_mgr);
    Check(reinserted.rows.size() == 1 && reinserted.rows[0][1].int_val == 11111,
          "after delete+reinsert of the same key, IndexScan returns exactly "
          "the new live row, "
          "not the dead one still sitting in the tree");
  }

  std::cout << "\n=== Part 9: index survives a restart ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(64, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    auto result = engine.Execute("SELECT * FROM accounts WHERE id = 25",
                                 nullptr, txn_mgr);
    Check(
        result.explain.find("IndexScan") != std::string::npos,
        "after a restart, WHERE id = 25 still uses IndexScan (index survived)");
    Check(result.rows.size() == 1 && result.rows[0][1].int_val == 2500,
          "and still returns the correct row");

    auto t = txn_mgr.Begin();
    engine.Execute("INSERT INTO accounts VALUES (5000, 500000)", t.get(),
                   txn_mgr);
    txn_mgr.Commit(t.get());
    auto new_row = engine.Execute("SELECT * FROM accounts WHERE id = 5000",
                                  nullptr, txn_mgr);
    Check(new_row.rows.size() == 1 && new_row.rows[0][1].int_val == 500000,
          "index maintenance still works for rows inserted after the restart");
  }

  CleanFiles();
  std::cout << "=== Part 10: aggregate SQL, and the columnar-vs-row planner "
               "decision ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(64, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    engine.Execute("CREATE TABLE sales (id INT, amount INT)", nullptr, txn_mgr);
    auto t = txn_mgr.Begin();
    for (int i = 1; i <= 20; ++i) {
      std::string sql = "INSERT INTO sales VALUES (" + std::to_string(i) +
                        ", " + std::to_string(i * 10) + ")";
      engine.Execute(sql, t.get(), txn_mgr);
    }
    txn_mgr.Commit(t.get());

    auto before =
        engine.Execute("SELECT SUM(amount) FROM sales", nullptr, txn_mgr);
    std::cout << "Before REFRESH: " << before.explain << "\n";
    Check(before.explain.find("RowAggregate") != std::string::npos,
          "before REFRESH COLUMNAR, SUM(amount) uses the row-store path");
    Check(before.rows.size() == 1 && before.rows[0][0].int_val == 2100,
          "row-path SUM is correct (1+2+...+20)*10 = 2100");

    engine.Execute("REFRESH COLUMNAR sales", nullptr, txn_mgr);

    auto after =
        engine.Execute("SELECT SUM(amount) FROM sales", nullptr, txn_mgr);
    std::cout << "After REFRESH: " << after.explain << "\n";
    Check(after.explain.find("ColumnarAggregate") != std::string::npos,
          "after REFRESH COLUMNAR, the SAME query now uses the columnar path");
    Check(after.rows.size() == 1 && after.rows[0][0].int_val == 2100,
          "columnar SUM matches the row-path result exactly");

    auto avg =
        engine.Execute("SELECT AVG(amount) FROM sales", nullptr, txn_mgr);
    Check(avg.explain.find("ColumnarAggregate") != std::string::npos,
          "AVG also uses columnar path");
    Check(avg.rows[0][0].text_val == "105.0000",
          "AVG(amount) = 2100/20 = 105.0000");

    auto cnt = engine.Execute("SELECT COUNT(*) FROM sales", nullptr, txn_mgr);
    Check(cnt.rows[0][0].int_val == 20,
          "COUNT(*) via columnar path is correct");

    auto mn = engine.Execute("SELECT MIN(amount) FROM sales", nullptr, txn_mgr);
    auto mx = engine.Execute("SELECT MAX(amount) FROM sales", nullptr, txn_mgr);
    Check(mn.rows[0][0].int_val == 10 && mx.rows[0][0].int_val == 200,
          "MIN/MAX via columnar path are correct");

    auto filtered = engine.Execute(
        "SELECT SUM(amount) FROM sales WHERE id > 15", nullptr, txn_mgr);
    std::cout << "Filtered aggregate: " << filtered.explain << "\n";
    Check(filtered.explain.find("RowAggregate") != std::string::npos,
          "a WHERE clause forces the row-store path even with a fresh columnar "
          "cache");
    Check(filtered.rows[0][0].int_val == (160 + 170 + 180 + 190 + 200),
          "filtered row-path SUM is still correct");
  }

  std::cout
      << "\n=== Part 11: staleness -- the columnar cache does NOT see new "
         "commits ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(64, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    engine.Execute("REFRESH COLUMNAR sales", nullptr, txn_mgr);
    auto baseline =
        engine.Execute("SELECT SUM(amount) FROM sales", nullptr, txn_mgr);
    Check(baseline.rows[0][0].int_val == 2100,
          "cache correctly rebuilt after restart");

    auto t = txn_mgr.Begin();
    engine.Execute("INSERT INTO sales VALUES (999, 100000)", t.get(), txn_mgr);
    txn_mgr.Commit(t.get());

    auto stale =
        engine.Execute("SELECT SUM(amount) FROM sales", nullptr, txn_mgr);
    std::cout << "Columnar SUM after an un-refreshed commit: "
              << stale.rows[0][0].int_val << " (row-store truth is now "
              << (2100 + 100000) << ")\n";
    Check(stale.rows[0][0].int_val == 2100,
          "columnar path returns the STALE pre-commit value -- this is the "
          "documented "
          "staleness window, not a bug");

    auto fresh_via_filter = engine.Execute(
        "SELECT SUM(amount) FROM sales WHERE id > 0", nullptr, txn_mgr);
    Check(fresh_via_filter.rows[0][0].int_val == 2100 + 100000,
          "the row-store path (forced here via a WHERE clause) sees the new "
          "commit immediately");

    engine.Execute("REFRESH COLUMNAR sales", nullptr, txn_mgr);
    auto refreshed =
        engine.Execute("SELECT SUM(amount) FROM sales", nullptr, txn_mgr);
    Check(refreshed.rows[0][0].int_val == 2100 + 100000,
          "after another REFRESH COLUMNAR, the columnar path catches up");
  }

  std::cout << "\n=== Part 12: zero-row edge case throws a clear error, not a "
               "wrong number ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(64, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    engine.Execute("CREATE TABLE empty_t (id INT, v INT)", nullptr, txn_mgr);
    engine.Execute("REFRESH COLUMNAR empty_t", nullptr, txn_mgr);

    bool threw = false;
    try {
      engine.Execute("SELECT SUM(v) FROM empty_t", nullptr, txn_mgr);
    } catch (const std::exception&) {
      threw = true;
    }
    Check(threw,
          "SUM over an empty table throws rather than silently returning 0");

    auto cnt = engine.Execute("SELECT COUNT(*) FROM empty_t", nullptr, txn_mgr);
    Check(cnt.rows[0][0].int_val == 0,
          "COUNT(*) over an empty table correctly returns 0, not an error");
  }

  std::cout << "\n=== Part 13: benchmark -- columnar vs. row-store SUM, same "
               "data, same answer ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(256, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    engine.Execute("CREATE TABLE big (id INT, amount INT)", nullptr, txn_mgr);
    constexpr int kRows = 5000;
    auto t = txn_mgr.Begin();
    for (int i = 0; i < kRows; ++i) {
      std::string sql = "INSERT INTO big VALUES (" + std::to_string(i) + ", " +
                        std::to_string(i) + ")";
      engine.Execute(sql, t.get(), txn_mgr);
    }
    txn_mgr.Commit(t.get());

    // Row-store timing (no columnar cache exists for 'big' yet).
    auto row_start = std::chrono::high_resolution_clock::now();
    auto row_result =
        engine.Execute("SELECT SUM(amount) FROM big", nullptr, txn_mgr);
    auto row_end = std::chrono::high_resolution_clock::now();
    double row_ms = duration_cast<std::chrono::duration<double, std::milli>>(
                        row_end - row_start)
                        .count();

    engine.Execute("REFRESH COLUMNAR big", nullptr, txn_mgr);

    auto col_start = std::chrono::high_resolution_clock::now();
    auto col_result =
        engine.Execute("SELECT SUM(amount) FROM big", nullptr, txn_mgr);
    auto col_end = std::chrono::high_resolution_clock::now();
    double col_ms = duration_cast<std::chrono::duration<double, std::milli>>(
                        col_end - col_start)
                        .count();

    std::cout << kRows << " rows: row-store SUM = " << row_ms
              << " ms, columnar SUM = " << col_ms << " ms\n";
    Check(row_result.rows[0][0].int_val == col_result.rows[0][0].int_val,
          "row-store and columnar paths agree exactly on the same SUM");
    Check(col_ms < row_ms,
          "columnar path is measurably faster than the row-store path for the "
          "same query");
  }

  CleanFiles();

  constexpr int kDim = 8;

  std::cout << "=== Part 14: CREATE TABLE with VECTOR, INSERT, CREATE VECTOR "
               "INDEX ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(64, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    engine.Execute("CREATE TABLE docs (id INT, embedding VECTOR(" +
                       std::to_string(kDim) + "))",
                   nullptr, txn_mgr);

    std::mt19937 rng(42);
    auto t = txn_mgr.Begin();
    std::vector<float> v0 = {1, 2, 3, 4, 5, 6, 7, 8};
    engine.Execute("INSERT INTO docs VALUES (0, " + VecLiteral(v0) + ")",
                   t.get(), txn_mgr);
    for (int i = 1; i < 40; ++i) {
      auto v = RandVec(rng, kDim);
      engine.Execute("INSERT INTO docs VALUES (" + std::to_string(i) + ", " +
                         VecLiteral(v) + ")",
                     t.get(), txn_mgr);
    }
    txn_mgr.Commit(t.get());

    bool threw_no_index = false;
    try {
      engine.Execute("SELECT id FROM docs ORDER BY embedding <-> " +
                         VecLiteral(v0) + " LIMIT 3",
                     nullptr, txn_mgr);
    } catch (const std::exception&) {
      threw_no_index = true;
    }
    Check(
        threw_no_index,
        "KNN query without a vector index throws a clear error (no fallback)");

    engine.Execute("CREATE VECTOR INDEX idx_emb ON docs (embedding) LISTS 4",
                   nullptr, txn_mgr);
    std::cout << "after creat\n";
    auto result = engine.Execute("SELECT id FROM docs ORDER BY embedding <-> " +
                                     VecLiteral(v0) + " LIMIT 3",
                                 nullptr, txn_mgr);
    std::cout << "Explain: " << result.explain << "\n";
    Check(result.explain.find("VectorIndexScan") != std::string::npos,
          "KNN query uses VectorIndexScan once an index exists");
    Check(!result.rows.empty() && result.rows[0][0].int_val == 0,
          "the nearest neighbor of v0 is v0 itself (id=0), correctly ranked "
          "first");
  }

  std::cout
      << "\n=== Part 15: hybrid search (WHERE + ORDER BY <-> LIMIT) ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(64, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    std::vector<float> query = {1, 2, 3, 4, 5, 6, 7, 8};
    auto filtered = engine.Execute(
        "SELECT id FROM docs WHERE id > 20 ORDER BY embedding <-> " +
            VecLiteral(query) + " LIMIT 5",
        nullptr, txn_mgr);
    std::cout << "Filtered explain: " << filtered.explain << "\n";
    Check(filtered.explain.find("PostFilter") != std::string::npos,
          "a WHERE clause combined with KNN shows up as a post-filter in the "
          "explain string");
    bool all_match_filter = true;
    for (auto& row : filtered.rows) {
      if (row[0].int_val <= 20) all_match_filter = false;
    }
    Check(all_match_filter,
          "every returned row actually satisfies the WHERE clause (id > 20)");
    std::cout << "Hybrid query returned " << filtered.rows.size()
              << " rows (asked for 5)\n";
  }

  std::cout << "\n=== Part 16: persistence across a restart ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(64, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    std::vector<float> v0 = {1, 2, 3, 4, 5, 6, 7, 8};
    auto result = engine.Execute("SELECT id FROM docs ORDER BY embedding <-> " +
                                     VecLiteral(v0) + " LIMIT 1",
                                 nullptr, txn_mgr);
    Check(!result.rows.empty() && result.rows[0][0].int_val == 0,
          "vector index survives a restart with no re-creation, and still "
          "ranks v0 first");

    auto t = txn_mgr.Begin();
    std::vector<float> v_new = {100, 100, 100, 100, 100, 100, 100, 100};
    engine.Execute("INSERT INTO docs VALUES (999, " + VecLiteral(v_new) + ")",
                   t.get(), txn_mgr);
    txn_mgr.Commit(t.get());
    auto post = engine.Execute("SELECT id FROM docs ORDER BY embedding <-> " +
                                   VecLiteral(v_new) + " LIMIT 1",
                               nullptr, txn_mgr);
    Check(!post.rows.empty() && post.rows[0][0].int_val == 999,
          "a row inserted after restart is findable via the (persisted, "
          "still-live) index");
  }

  std::cout << "\n=== Part 17: recall@k evaluation, varying nprobe -- via a "
               "larger, fresh index ===\n";
  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    RecoveryManager(&disk, &wal).Recover();
    BufferPool pool(300, &disk, &wal, 8);
    TransactionManager txn_mgr(&wal);
    Engine engine(&disk, &pool, &wal);

    engine.Execute("CREATE TABLE embeddings (id INT, v VECTOR(" +
                       std::to_string(kDim) + "))",
                   nullptr, txn_mgr);

    std::mt19937 rng(7);
    std::vector<std::pair<std::vector<float>, int64_t>> ground_truth_data;
    auto t = txn_mgr.Begin();
    constexpr int kNumVectors = 800;
    for (int i = 0; i < kNumVectors; ++i) {
      auto v = RandVec(rng, kDim);
      engine.Execute("INSERT INTO embeddings VALUES (" + std::to_string(i) +
                         ", " + VecLiteral(v) + ")",
                     t.get(), txn_mgr);
      ground_truth_data.push_back({v, i});
    }
    txn_mgr.Commit(t.get());
    engine.Execute("CREATE VECTOR INDEX idx_v ON embeddings (v) LISTS 20",
                   nullptr, txn_mgr);

    auto query = RandVec(rng, kDim);
    constexpr uint32_t kEvalK = 10;
    auto exact_ids = BruteForceKNN(ground_truth_data, query, kEvalK);

    auto raw_index = engine.GetCatalog().GetVectorIndex("embeddings", "v");
    std::cout << "nprobe : recall@" << kEvalK << "\n";
    for (uint32_t nprobe : {1u, 2u, 4u, 8u, 20u}) {
      auto approx_rids = raw_index->Search(query, kEvalK, nprobe);
      std::vector<int64_t> approx_ids;
      for (auto& rid : approx_rids) {
        auto heap = engine.GetCatalog().GetHeap("embeddings");
        TupleHeader hdr;
        std::vector<char> bytes;
        if (heap->ReadTuple(rid, &hdr, &bytes)) {
          auto values = DeserializeRow(
              *engine.GetCatalog().GetSchema("embeddings"), bytes);
          approx_ids.push_back(values[0].int_val);
        }
      }
      double recall = RecallAtK(approx_ids, exact_ids);
      std::cout << "  " << nprobe << " : " << recall << "\n";
    }

    auto full = raw_index->Search(query, kEvalK, 20);
    Check(full.size() == kEvalK,
          "full-nprobe search returns exactly k candidates for a table this "
          "size");
  }

  std::cout << "\n=== Summary ===\n";
  if (failures == 0)
    std::cout << "ALL CHECKS PASSED\n";
  else
    std::cout << failures << " CHECKS FAILED\n";
  CleanFiles();
  return failures == 0 ? 0 : 1;
}