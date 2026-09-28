#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "network_utils.h"

namespace minidb {
class Launcher {
 private:
  struct ServerInfo {
    int id;
    int query_port;
    int control_port;
    pid_t pid = -1;
    bool alive = true;
  };
  int num_servers;
  int threads_per_server;
  std::string data_dir;
  int base_port;
  int client_port;
  std::mutex topology_mutex;
  std::vector<ServerInfo> servers;
  int primary_id = 0;
  uint32_t current_epoch = 1;
  std::atomic<bool> running{true};
  std::atomic<int> next_read_target{0};
  std::thread heartbeat_thread;
  net::PersistentConnection heartbeat_conn;
  void SpawnServer(int id);
  void BroadcastSetPrimary(int primary_id, const std::vector<int>& ids);
  void PromoteServer(int id, uint32_t epoch,
                     const std::vector<int>& replica_ids);
  std::string SendQueryToServer(int id, const std::string& sql);
  std::string SendControlToServer(int id, const std::string& line);
  std::string QueryServerPersistent(
      std::vector<std::unique_ptr<net::PersistentConnection>>& conns, int id,
      const std::string& sql);
  void HeartbeatLoop();
  void HandleElection();
  void HandleClientConnection(int fd);

 public:
  Launcher(int num_servers, int threads_per_server, const std::string& data_dir,
           int base_port, int client_port);
  ~Launcher();

  void Run();
};
}  // namespace minidb