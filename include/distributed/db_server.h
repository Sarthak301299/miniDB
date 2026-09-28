#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
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