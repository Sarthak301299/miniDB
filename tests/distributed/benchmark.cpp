#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "network_utils.h"

using namespace minidb::net;
using namespace std::chrono;

namespace {

std::string ArgValue(int argc, char** argv, const std::string& flag,
                     const std::string& def) {
  std::string prefix = "--" + flag + "=";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
  }
  return def;
}

std::string VecLiteral(const std::vector<float>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += std::to_string(v[i]);
  }
  return s + "]";
}

std::vector<float> RandVec(std::mt19937& rng, int dim) {
  std::uniform_real_distribution<float> dist(0.0f, 100.0f);
  std::vector<float> v(dim);
  for (auto& x : v) x = dist(rng);
  return v;
}

constexpr int kVecDim = 8;
std::string RunQuery(PersistentConnection& conn, const std::string& sql) {
  return conn.SendAndRecv(sql);
}

enum class QueryKind {
  kRelationalInsert,
  kRelationalPointRead,
  kAnalytical,
  kVector
};

struct Counters {
  std::atomic<int64_t> relational_insert{0};
  std::atomic<int64_t> relational_read{0};
  std::atomic<int64_t> analytical{0};
  std::atomic<int64_t> vector{0};
  std::atomic<int64_t> errors{0};
};

void WorkerThread(int port, int queries_per_thread, int thread_id,
                  int seed_row_count, Counters* counters,
                  std::atomic<int64_t>* next_relational_id, bool read_only) {
  PersistentConnection conn;
  conn.SetTarget("127.0.0.1", port);
  std::mt19937 rng(1000 + thread_id);
  std::uniform_int_distribution<int> kind_dist(0, 99);
  std::uniform_int_distribution<int> existing_id_dist(0, seed_row_count - 1);
  std::uniform_int_distribution<int> agg_choice(0, 4);
  static const char* kAggFuncs[] = {"SUM", "AVG", "COUNT", "MIN", "MAX"};

  for (int i = 0; i < queries_per_thread; ++i) {
    int r = kind_dist(rng);
    QueryKind kind;
    if (read_only) {
      if (r < 50)
        kind = QueryKind::kRelationalPointRead;
      else if (r < 75)
        kind = QueryKind::kAnalytical;
      else
        kind = QueryKind::kVector;
    } else {
      if (r < 40)
        kind = QueryKind::kRelationalInsert;
      else if (r < 70)
        kind = QueryKind::kRelationalPointRead;
      else if (r < 85)
        kind = QueryKind::kAnalytical;
      else
        kind = QueryKind::kVector;
    }

    std::string sql;
    switch (kind) {
      case QueryKind::kRelationalInsert: {
        int64_t new_id = next_relational_id->fetch_add(1);
        sql = "INSERT INTO bench_rel VALUES (" + std::to_string(new_id) + ", " +
              std::to_string(static_cast<int>(new_id % 1000)) + ")";
        break;
      }
      case QueryKind::kRelationalPointRead: {
        int id = existing_id_dist(rng);
        sql = "SELECT * FROM bench_rel WHERE id = " + std::to_string(id);
        break;
      }
      case QueryKind::kAnalytical: {
        sql = std::string("SELECT ") + kAggFuncs[agg_choice(rng)] +
              "(amount) FROM bench_analytics";
        break;
      }
      case QueryKind::kVector: {
        sql = "SELECT id FROM bench_vectors ORDER BY embedding <-> " +
              VecLiteral(RandVec(rng, kVecDim)) + " LIMIT 5";
        break;
      }
    }

    std::string resp = RunQuery(conn, sql);
    if (resp.rfind("OK", 0) != 0) {
      static std::atomic<int> printed{0};
      if (printed.fetch_add(1) < 5) {
        std::cerr << "[benchmark DEBUG] query failed: \"" << sql << "\" -> \""
                  << resp << "\"\n";
      }
      counters->errors.fetch_add(1);
    } else {
      switch (kind) {
        case QueryKind::kRelationalInsert:
          counters->relational_insert.fetch_add(1);
          break;
        case QueryKind::kRelationalPointRead:
          counters->relational_read.fetch_add(1);
          break;
        case QueryKind::kAnalytical:
          counters->analytical.fetch_add(1);
          break;
        case QueryKind::kVector:
          counters->vector.fetch_add(1);
          break;
      }
    }
  }
}

}  // namespace

// Usage: benchmark --port=P --clients=C --queries-per-client=Q
// [--seed-rows=200]
int main(int argc, char** argv) {
  int port = std::stoi(ArgValue(argc, argv, "port", "19999"));
  int clients = std::stoi(ArgValue(argc, argv, "clients", "4"));
  int queries_per_client =
      std::stoi(ArgValue(argc, argv, "queries-per-client", "50"));
  int seed_rows = std::stoi(ArgValue(argc, argv, "seed-rows", "200"));
  bool read_only = ArgValue(argc, argv, "read-only", "false") ==
                   "true";  // >>> READ-ONLY MODE: NEW >>> <<<

  std::cerr << "[benchmark] setup: creating tables and seeding " << seed_rows
            << " rows...\n";

  PersistentConnection setup_conn;
  setup_conn.SetTarget("127.0.0.1", port);
  RunQuery(setup_conn, "CREATE TABLE bench_rel (id INT, val INT)");
  RunQuery(setup_conn, "CREATE INDEX idx_bench_rel_id ON bench_rel (id)");
  RunQuery(setup_conn, "CREATE TABLE bench_analytics (id INT, amount INT)");
  RunQuery(setup_conn, "CREATE TABLE bench_vectors (id INT, embedding VECTOR(" +
                           std::to_string(kVecDim) + "))");

  std::mt19937 setup_rng(42);
  for (int i = 0; i < seed_rows; ++i) {
    RunQuery(setup_conn, "INSERT INTO bench_rel VALUES (" + std::to_string(i) +
                             ", " + std::to_string(i % 1000) + ")");
    RunQuery(setup_conn, "INSERT INTO bench_analytics VALUES (" +
                             std::to_string(i) + ", " +
                             std::to_string((i * 37) % 5000) + ")");
    RunQuery(setup_conn, "INSERT INTO bench_vectors VALUES (" +
                             std::to_string(i) + ", " +
                             VecLiteral(RandVec(setup_rng, kVecDim)) + ")");
  }
  RunQuery(setup_conn, "REFRESH COLUMNAR bench_analytics");
  RunQuery(
      setup_conn,
      "CREATE VECTOR INDEX idx_bench_vec ON bench_vectors (embedding) LISTS " +
          std::to_string(std::max(2, seed_rows / 20)));

  std::cerr << "[benchmark] setup complete. Running " << clients
            << " client thread(s) x " << queries_per_client
            << " queries each...\n";

  Counters counters;
  std::atomic<int64_t> next_relational_id{seed_rows};
  std::vector<std::thread> threads;

  auto start = high_resolution_clock::now();
  for (int t = 0; t < clients; ++t) {
    threads.emplace_back(WorkerThread, port, queries_per_client, t, seed_rows,
                         &counters, &next_relational_id, read_only);
  }
  for (auto& th : threads) th.join();
  auto end = high_resolution_clock::now();

  double seconds = duration_cast<duration<double>>(end - start).count();
  int64_t total_ok = counters.relational_insert.load() +
                     counters.relational_read.load() +
                     counters.analytical.load() + counters.vector.load();
  int64_t total = total_ok + counters.errors.load();

  std::cout << "=== Benchmark Results ===\n";
  std::cout << "clients=" << clients
            << " queries_per_client=" << queries_per_client
            << " total_queries=" << total
            << " read_only=" << (read_only ? "true" : "false") << "\n";
  std::cout << "elapsed_seconds=" << seconds << "\n";
  std::cout << "throughput_qps=" << (static_cast<double>(total_ok) / seconds)
            << "\n";
  std::cout << "relational_insert=" << counters.relational_insert.load()
            << " relational_read=" << counters.relational_read.load()
            << " analytical=" << counters.analytical.load()
            << " vector=" << counters.vector.load()
            << " errors=" << counters.errors.load() << "\n";

  return 0;
}