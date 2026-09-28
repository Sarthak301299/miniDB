#include "launcher.h"

#include <fcntl.h>
#include <libgen.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>

#include "network_utils.h"

namespace minidb {
namespace {
std::string Upper(const std::string& s) {
  std::string r = s;
  for (auto& c : r)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}
}  // namespace

void Launcher::SpawnServer(int id) {
  pid_t pid = fork();
  if (pid < 0) throw std::runtime_error("Launcher: fork() failed");
  if (pid == 0) {
    auto& info = servers[static_cast<size_t>(id)];
    std::string id_arg = "--id=" + std::to_string(id);
    std::string dir_arg = "--data-dir=" + data_dir;
    std::string threads_arg = "--threads=" + std::to_string(threads_per_server);
    std::string qport_arg = "--query-port=" + std::to_string(info.query_port);
    std::string cport_arg =
        "--control-port=" + std::to_string(info.control_port);
    char result[PATH_MAX];
    readlink("/proc/self/exe", result, PATH_MAX);
    std::string app_dir = dirname(result);
    std::string server_path = app_dir + "/server";
    execl(server_path.c_str(), "server", id_arg.c_str(), dir_arg.c_str(),
          threads_arg.c_str(), qport_arg.c_str(), cport_arg.c_str(),
          static_cast<char*>(nullptr));
    std::cerr << "Launcher: execlp failed for server " << id << ": "
              << std::strerror(errno) << "\n";
    _exit(1);
  }
  servers[static_cast<size_t>(id)].pid = pid;
}

std::string Launcher::SendQueryToServer(int id, const std::string& sql) {
  auto& info = servers[static_cast<size_t>(id)];
  int fd = net::ConnectWithRetry("127.0.0.1", info.query_port, 10);
  net::SendLine(fd, "QUERY " + sql);
  std::string response;
  net::RecvLine(fd, &response);
  net::CloseSocket(fd);
  return response;
}

std::string Launcher::SendControlToServer(int id, const std::string& line) {
  auto& info = servers[static_cast<size_t>(id)];
  int fd = net::ConnectWithRetry("127.0.0.1", info.control_port, 10);
  net::SendLine(fd, line);
  std::string response;
  net::RecvLine(fd, &response);
  net::CloseSocket(fd);
  return response;
}

void Launcher::PromoteServer(int id, uint32_t epoch,
                             const std::vector<int>& replica_ids) {
  std::string peer_list;
  for (size_t i = 0; i < replica_ids.size(); ++i) {
    if (i) peer_list += ",";
    auto& r = servers[static_cast<size_t>(replica_ids[i])];
    peer_list += "127.0.0.1:" + std::to_string(r.control_port);
  }
  std::string resp = SendControlToServer(
      id, "PROMOTE " + std::to_string(epoch) + " " + peer_list);
  std::cerr << "[launcher] promoted server " << id << " to primary (epoch "
            << epoch << "), response: " << resp << "\n";
}

void Launcher::BroadcastSetPrimary(int primary_id,
                                   const std::vector<int>& ids) {
  auto& p = servers[static_cast<size_t>(primary_id)];
  std::string addr = "127.0.0.1:" + std::to_string(p.control_port);
  for (int id : ids) {
    try {
      SendControlToServer(id, "SETPRIMARY " + addr);
    } catch (const std::exception& e) {
      std::cerr << "[launcher] SETPRIMARY to server " << id
                << " failed: " << e.what() << "\n";
    }
  }
}

void Launcher::HeartbeatLoop() {
  constexpr int failure_threshold = 3;
  int consecutive_failures = 0;
  while (running.load()) {
    try {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      int current_primary;
      {
        std::lock_guard<std::mutex> lock(topology_mutex);
        current_primary = primary_id;
      }
      bool ok = false;
      try {
        std::string resp = heartbeat_conn.SendAndRecv("PING");
        ok = (resp == "PONG");
      } catch (const std::exception&) {
        ok = false;
      }
      if (ok) {
        consecutive_failures = 0;
      } else {
        consecutive_failures++;
        std::cerr << "[launcher] heartbeat to primary " << current_primary
                  << " failed (" << consecutive_failures << "/"
                  << failure_threshold << ")\n";
        if (consecutive_failures >= failure_threshold) {
          HandleElection();
          consecutive_failures = 0;
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "[launcher] heartbeat loop: unexpected error, continuing: "
                << e.what() << "\n";
    }
  }
}

void Launcher::HandleElection() {
  std::lock_guard<std::mutex> lock(topology_mutex);
  int dead_primary = primary_id;
  servers[static_cast<size_t>(dead_primary)].alive = false;
  std::cerr << "[launcher] ELECTION: primary " << dead_primary
            << " presumed dead\n";

  int best_id = -1;
  for (auto& s : servers) {
    if (s.alive && s.id != dead_primary) {
      best_id = s.id;
      break;
    }
  }

  if (best_id < 0) {
    std::cerr << "[launcher] ELECTION FAILED: no surviving server to promote\n";
    return;
  }

  try {
    current_epoch++;
    std::vector<int> remaining_replicas;
    for (auto& s : servers) {
      if (s.alive && s.id != best_id) remaining_replicas.push_back(s.id);
    }
    PromoteServer(best_id, current_epoch, remaining_replicas);
    primary_id = best_id;
    heartbeat_conn.SetTarget(
        "127.0.0.1", servers[static_cast<size_t>(best_id)].control_port);
    BroadcastSetPrimary(best_id, remaining_replicas);
    std::cerr << "[launcher] ELECTION COMPLETE: server " << best_id
              << " is now primary (epoch " << current_epoch << ")\n";
  } catch (const std::exception& e) {
    std::cerr << "[launcher] ELECTION FAILED: could not promote server "
              << best_id << ": " << e.what()
              << " -- will retry on the next heartbeat cycle\n";
  }
}

std::string Launcher::QueryServerPersistent(
    std::vector<std::unique_ptr<net::PersistentConnection>>& conns, int id,
    const std::string& sql) {
  if (!conns[static_cast<size_t>(id)]) {
    conns[static_cast<size_t>(id)] =
        std::make_unique<net::PersistentConnection>();
    conns[static_cast<size_t>(id)]->SetTarget(
        "127.0.0.1", servers[static_cast<size_t>(id)].query_port);
  }
  return conns[static_cast<size_t>(id)]->SendAndRecv("QUERY " + sql);
}

void Launcher::HandleClientConnection(int fd) {
  std::vector<std::unique_ptr<net::PersistentConnection>> query_conns(
      static_cast<size_t>(num_servers));
  while (true) {
    std::string line;
    if (!net::RecvLine(fd, &line)) break;

    std::string upper = Upper(line);
    bool touches_wal = upper.rfind("INSERT", 0) == 0 ||
                       upper.rfind("DELETE", 0) == 0 ||
                       upper.rfind("CREATE", 0) == 0;

    std::string response;
    try {
      if (touches_wal) {
        int primary;
        {
          std::lock_guard<std::mutex> lock(topology_mutex);
          primary = primary_id;
        }
        response = QueryServerPersistent(query_conns, primary, line);
      } else {
        std::vector<int> alive_ids;
        {
          std::lock_guard<std::mutex> lock(topology_mutex);
          for (auto& s : servers) {
            if (s.alive) alive_ids.push_back(s.id);
          }
        }
        if (alive_ids.empty()) {
          response = "ERR no servers available";
        } else {
          int idx = next_read_target.fetch_add(1) %
                    static_cast<int>(alive_ids.size());
          response = QueryServerPersistent(
              query_conns, alive_ids[static_cast<size_t>(idx)], line);
        }
      }
    } catch (const std::exception& e) {
      response = std::string("ERR launcher routing failed: ") + e.what();
    }

    if (!net::SendLine(fd, response)) break;
  }
  net::CloseSocket(fd);
}

Launcher::Launcher(int num_servers, int threads_per_server,
                   const std::string& data_dir, int base_port, int client_port)
    : num_servers(num_servers),
      threads_per_server(threads_per_server),
      data_dir(data_dir),
      base_port(base_port),
      client_port(client_port) {
  std::string db_path = data_dir + "/db_file.db";
  std::string wal_path = data_dir + "/log_file.wal";
  int fd1 = ::open(db_path.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd1 >= 0) ::close(fd1);
  int fd2 = ::open(wal_path.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd2 >= 0) ::close(fd2);
  for (int i = 0; i < num_servers; ++i) {
    ServerInfo info;
    info.id = i;
    info.query_port = base_port + i * 2;
    info.control_port = base_port + i * 2 + 1;
    servers.push_back(info);
  }
  for (int i = 0; i < num_servers; ++i) SpawnServer(i);
  for (int i = 0; i < num_servers; ++i) {
    bool up = false;
    for (int attempt = 0; attempt < 200 && !up; ++attempt) {
      try {
        up = (SendControlToServer(i, "PING") == "PONG");
      } catch (const std::exception&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
    if (!up)
      throw std::runtime_error("Launcher: server " + std::to_string(i) +
                               " never came up");
  }
  std::vector<int> initial_replicas;
  for (int i = 1; i < num_servers; ++i) initial_replicas.push_back(i);
  PromoteServer(0, current_epoch, initial_replicas);
  primary_id = 0;
  BroadcastSetPrimary(0, initial_replicas);
  heartbeat_conn.SetTarget("127.0.0.1", servers[0].control_port);
  heartbeat_thread = std::thread([this] { HeartbeatLoop(); });
}

Launcher::~Launcher() {
  running.store(false);
  if (heartbeat_thread.joinable()) heartbeat_thread.join();
  for (auto& s : servers) {
    if (s.pid > 0) {
      ::kill(s.pid, SIGTERM);
      int status;
      ::waitpid(s.pid, &status, 0);
    }
  }
}

void Launcher::Run() {
  int listen_fd = net::CreateListenSocket(client_port);
  std::cerr << "[launcher] listening for clients on port " << client_port
            << "\n";
  while (running.load()) {
    int fd = net::AcceptConnection(listen_fd);
    if (fd < 0) continue;
    std::thread(&Launcher::HandleClientConnection, this, fd).detach();
  }
}

}  // namespace minidb