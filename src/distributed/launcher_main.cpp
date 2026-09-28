#include <iostream>
#include <string>

#include "launcher.h"

using namespace minidb;

namespace {
std::string ArgValue(int argc, char** argv, const std::string& flag,
                     const std::string& def = "") {
  std::string prefix = "--" + flag + "=";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
  }
  if (!def.empty()) return def;
  throw std::runtime_error("Missing required argument --" + flag);
}
}  // namespace

// Usage: launcher --servers=N --threads=T --data-dir=DIR
//                 [--base-port=20000] [--client-port=19999]
int main(int argc, char** argv) {
  try {
    int num_servers = std::stoi(ArgValue(argc, argv, "servers"));
    int threads = std::stoi(ArgValue(argc, argv, "threads"));
    std::string data_dir = ArgValue(argc, argv, "data-dir");
    int base_port = std::stoi(ArgValue(argc, argv, "base-port", "20000"));
    int client_port = std::stoi(ArgValue(argc, argv, "client-port", "19999"));

    std::cerr << "[launcher] starting " << num_servers << " server(s), "
              << threads << " thread(s) each, data_dir=" << data_dir << "\n";

    Launcher launcher(num_servers, threads, data_dir, base_port, client_port);
    launcher.Run();
  } catch (const std::exception& e) {
    std::cerr << "launcher: fatal error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}