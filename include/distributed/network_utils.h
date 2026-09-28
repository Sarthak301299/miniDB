#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace minidb {
namespace net {
int CreateListenSocket(int port);
int AcceptConnection(int listen_fd);
int ConnectWithRetry(const std::string& host, int port, int max_attempts = 50);
void CloseSocket(int fd);

bool SendLine(int fd, const std::string& line);
bool RecvLine(int fd, std::string* out);

bool SendFramed(int fd, const std::vector<char>& data);
bool RecvFramed(int fd, std::vector<char>* out);

class PersistentConnection {
 private:
  std::mutex mutex;
  int fd = -1;
  std::string host;
  int port = -1;

 public:
  PersistentConnection() = default;
  ~PersistentConnection();
  PersistentConnection(const PersistentConnection&) = delete;
  PersistentConnection& operator=(const PersistentConnection&) = delete;
  void SetTarget(const std::string& host, int port);
  std::string SendAndRecv(const std::string& line);
  static std::vector<std::string> SendAndRecvAll(
      const std::vector<PersistentConnection*>& conns, const std::string& line);
};
}  // namespace net
}  // namespace minidb