#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "db_server.h"

using namespace minidb;

namespace {
std::string ArgValue(int argc, char** argv, const std::string& flag) {
  std::string prefix = "--" + flag + "=";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
  }
  throw std::runtime_error("Missing required argument --" + flag);
}
}  // namespace

// Usage: server --id=N --data-dir=DIR --threads=T --query-port=P
// --control-port=C
int main(int argc, char** argv) {
  try {
    int id = std::stoi(ArgValue(argc, argv, "id"));
    std::string data_dir = ArgValue(argc, argv, "data-dir");
    int threads = std::stoi(ArgValue(argc, argv, "threads"));
    int query_port = std::stoi(ArgValue(argc, argv, "query-port"));
    int control_port = std::stoi(ArgValue(argc, argv, "control-port"));

    std::cerr << "[server " << id << "] starting: data_dir=" << data_dir
              << " threads=" << threads << " query_port=" << query_port
              << " control_port=" << control_port << "\n";

    DbServer server(id, data_dir, threads, query_port, control_port);
    server.Run();
  } catch (const std::exception& e) {
    std::cerr << "server: fatal error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}