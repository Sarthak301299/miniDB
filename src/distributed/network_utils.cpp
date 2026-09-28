#include "network_utils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace minidb {
namespace net {
int CreateListenSocket(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("CreateListenSocket: socket() failed");

  int opt = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(port));

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    throw std::runtime_error("CreateListenSocket: bind() failed on port " +
                             std::to_string(port));
  }
  if (::listen(fd, 64) < 0) {
    ::close(fd);
    throw std::runtime_error("CreateListenSocket: listen() failed");
  }
  return fd;
}

int AcceptConnection(int listen_fd) {
  sockaddr_in client_addr{};
  socklen_t len = sizeof(client_addr);
  int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
  return fd;
}

int ConnectWithRetry(const std::string& host, int port, int max_attempts) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("ConnectWithRetry: socket() failed");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      return fd;
    }
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("ConnectWithRetry: could not connect to " + host +
                           ":" + std::to_string(port) + " after " +
                           std::to_string(max_attempts) + " attempts");
}

void CloseSocket(int fd) {
  if (fd >= 0) ::close(fd);
}

bool SendLine(int fd, const std::string& line) {
  std::string msg = line + "\n";
  size_t sent = 0;
  while (sent < msg.size()) {
    ssize_t n = ::send(fd, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool RecvLine(int fd, std::string* out) {
  out->clear();
  char c;
  while (true) {
    ssize_t n = ::recv(fd, &c, 1, 0);
    if (n <= 0) return false;
    if (c == '\n') return true;
    out->push_back(c);
  }
}

namespace {
bool SendAll(int fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}
bool RecvAll(int fd, char* data, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = ::recv(fd, data + got, len - got, 0);
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
  }
  return true;
}
}  // namespace

bool SendFramed(int fd, const std::vector<char>& data) {
  uint32_t len_be = htonl(static_cast<uint32_t>(data.size()));
  if (!SendAll(fd, reinterpret_cast<const char*>(&len_be), sizeof(len_be)))
    return false;
  if (data.empty()) return true;
  return SendAll(fd, data.data(), data.size());
}

bool RecvFramed(int fd, std::vector<char>* out) {
  uint32_t len_be = 0;
  if (!RecvAll(fd, reinterpret_cast<char*>(&len_be), sizeof(len_be)))
    return false;
  uint32_t len = ntohl(len_be);
  out->resize(len);
  if (len == 0) return true;
  return RecvAll(fd, out->data(), len);
}

PersistentConnection::~PersistentConnection() {
  if (fd >= 0) CloseSocket(fd);
}

void PersistentConnection::SetTarget(const std::string& host_, int port_) {
  std::lock_guard<std::mutex> lock(mutex);
  if (fd >= 0) {
    CloseSocket(fd);
    fd = -1;
  }
  host = host_;
  port = port_;
}

std::string PersistentConnection::SendAndRecv(const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex);
  if (port < 0) {
    throw std::runtime_error(
        "PersistentConnection: SendAndRecv called before SetTarget");
  }

  auto try_once = [&](int fd) -> std::pair<bool, std::string> {
    if (!SendLine(fd, line)) return {false, ""};
    std::string resp;
    if (!RecvLine(fd, &resp)) return {false, ""};
    return {true, resp};
  };

  if (fd < 0) {
    fd = ConnectWithRetry(host, port, 10);
  }

  auto [ok, response] = try_once(fd);
  if (!ok) {
    CloseSocket(fd);
    fd = ConnectWithRetry(host, port, 10);
    auto [ok2, response2] = try_once(fd);
    if (!ok2) {
      CloseSocket(fd);
      fd = -1;
      throw std::runtime_error("PersistentConnection: request to " + host +
                               ":" + std::to_string(port) +
                               " failed even after reconnecting");
    }
    return response2;
  }
  return response;
}

std::vector<std::string> PersistentConnection::SendAndRecvAll(
    const std::vector<PersistentConnection*>& conns, const std::string& line) {
  const size_t n = conns.size();
  std::vector<std::string> replies(n);
  if (n == 0) return replies;
  std::vector<PersistentConnection*> lock_order(conns);
  std::sort(lock_order.begin(), lock_order.end());
  lock_order.erase(std::unique(lock_order.begin(), lock_order.end()),
                   lock_order.end());
  std::vector<std::unique_lock<std::mutex>> locks;
  locks.reserve(lock_order.size());
  for (auto* c : lock_order) locks.emplace_back(c->mutex);
  std::vector<std::string> errors(n);
  std::vector<bool> sent(n, false);
  for (size_t i = 0; i < n; ++i) {
    PersistentConnection* c = conns[i];
    if (c->port < 0) {
      errors[i] =
          "PersistentConnection: SendAndRecvAll called before SetTarget";
      continue;
    }
    try {
      if (c->fd < 0) c->fd = ConnectWithRetry(c->host, c->port, 10);
      sent[i] = SendLine(c->fd, line);
    } catch (const std::exception&) {
      c->fd = -1;
      sent[i] = false;
    }
  }
  for (size_t i = 0; i < n; ++i) {
    PersistentConnection* c = conns[i];
    if (!errors[i].empty()) continue;
    if (sent[i] && RecvLine(c->fd, &replies[i])) continue;
    if (c->fd >= 0) CloseSocket(c->fd);
    c->fd = -1;
    try {
      c->fd = ConnectWithRetry(c->host, c->port, 10);
      if (!SendLine(c->fd, line) || !RecvLine(c->fd, &replies[i])) {
        CloseSocket(c->fd);
        c->fd = -1;
        errors[i] = "PersistentConnection: request to " + c->host + ":" +
                    std::to_string(c->port) + " failed even after reconnecting";
      }
    } catch (const std::exception& e) {
      c->fd = -1;
      errors[i] = e.what();
    }
  }
  for (size_t i = 0; i < n; ++i) {
    if (!errors[i].empty()) throw std::runtime_error(errors[i]);
  }
  return replies;
}

}  // namespace net
}  // namespace minidb