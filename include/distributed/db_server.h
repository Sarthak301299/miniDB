#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "buffer_pool.h"
#include "disk_manager.h"
#include "engine.h"
#include "network_utils.h"
#include "transaction_manager.h"
#include "wal_manager.h"

namespace minidb {
constexpr int numControlThreads = 16;

class DbServer {
 private:
  int id;
  std::string data_dir;
  int num_threads;
  int query_port;
  int control_port;
  std::string db_path, wal_path, lock_path;
  std::atomic<bool> is_primary{false};
  std::atomic<uint32_t> epoch{0};
  int primary_lock_fd = -1;

  mutable std::shared_mutex engine_mutex;  // see class comment
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<WALManager> wal;
  std::unique_ptr<BufferPool> pool;
  std::unique_ptr<TransactionManager> txn_mgr;
  std::unique_ptr<Engine> engine;

  std::vector<std::pair<std::string, int>> replica_control_addrs;
  std::vector<std::unique_ptr<net::PersistentConnection>> replica_conns;
  std::atomic<long> last_synced_offset{0};
  std::atomic<LSN> sync_max_lsn{0};
  std::mutex mark_mutex;
  long mark_offset = 0;
  std::atomic<LSN> mark_max_lsn{0};
  std::string current_primary_host;
  int current_primary_control_port = -1;

  static constexpr size_t connection_pool_size = 4;
  std::vector<std::unique_ptr<net::PersistentConnection>> primary_conn_pool;
  std::atomic<size_t> next_primary_conn{0};

  std::mutex pending_flush_mutex;
  std::unordered_set<PageId> pending_flush_pages;

  int query_listen_fd = -1;
  int control_listen_fd = -1;

  struct WALLoc {
    long offset;
    LSN lsn;
  };
  std::mutex wal_index_mutex;
  std::unordered_map<PageId, WALLoc> wal_index;
  bool wal_mode = true;
  std::atomic<uint64_t> wal_page_reads{0};
  std::atomic<uint64_t> disk_page_reads{0};
  std::atomic<uint64_t> flush_requests{0};

  void DoNotifyRound();
  std::mutex notify_mu;
  std::condition_variable notify_cv;
  uint64_t notify_rounds_started = 0;
  uint64_t notify_rounds_completed = 0;
  bool notify_round_in_flight = false;
  struct NotifyErr {
    uint64_t round = 0;
    std::string msg;
  };
  static constexpr size_t notify_err_ring = 256;
  NotifyErr notify_errors[notify_err_ring];
  std::atomic<uint64_t> notify_calls{0};
  std::atomic<uint64_t> notify_rounds{0};
  std::atomic<uint64_t> notify_busy_ns{0};

  void ServeWithEpoll(int listen_fd, int threads,
                      std::function<std::string(const std::string&)> handler);
  std::string ProcessQuery(const std::string& line);
  std::string ProcessControlMessage(const std::string& line);
  std::string RunAutocommit(const std::string& sql);
  void ControlListenerLoop();
  void HandleControlConnection(int fd);
  void NotifyReplicasAndWait();
  void SyncFromSharedWal();
  void RequestPrimaryFlushPage(PageId page_id);
  void PreDiskReadHook(PageId page_id);
  void PostDiskReadHook(PageId page_id, LSN page_lsn);
  bool WALPageProvider(PageId page_id, Page* page);
  void IndexWALRecords(const std::vector<WALRecord>& records,
                       const std::vector<long>& offsets);
  void BuildEngineStackLocked(bool read_only);

 public:
  DbServer(int id, std::string data_dir, int num_threads, int query_port,
           int control_port);

  void Run();
  void BecomePrimary(
      uint32_t epoch,
      std::vector<std::pair<std::string, int>> replica_control_addrs);
};
}  // namespace minidb