#include "client/client_config.h"
#include "common/logging.h"

#include <yaml-cpp/yaml.h>

namespace anycache {

ClientConfig ClientConfig::Default() {
  ClientConfig cfg;
  return cfg;
}

ClientConfig ClientConfig::LoadFromFile(const std::string &path) {
  try {
    YAML::Node root = YAML::LoadFile(path);
    ClientConfig cfg = Default();

    auto client = root["client"];
    auto fuse = root["fuse"];
    auto master = root["master"];

    if (client) {
      if (client["master_address"])
        cfg.master_address = client["master_address"].as<std::string>();
      if (client["master_rpc_timeout_ms"])
        cfg.master_rpc_timeout_ms = client["master_rpc_timeout_ms"].as<int>();
      if (client["worker_rpc_timeout_ms"])
        cfg.worker_rpc_timeout_ms = client["worker_rpc_timeout_ms"].as<int>();
    } else if (fuse && fuse["master_address"]) {
      cfg.master_address = fuse["master_address"].as<std::string>();
    } else if (master && master["host"] && master["port"]) {
      cfg.master_address = master["host"].as<std::string>() + ":" +
                           std::to_string(master["port"].as<int>());
    }

    auto rpc = root["rpc"];
    if (rpc) {
      if (rpc["master_rpc_timeout_ms"])
        cfg.master_rpc_timeout_ms = rpc["master_rpc_timeout_ms"].as<int>();
      if (rpc["worker_rpc_timeout_ms"])
        cfg.worker_rpc_timeout_ms = rpc["worker_rpc_timeout_ms"].as<int>();
    }

    return cfg;
  } catch (const std::exception &e) {
    LOG_WARN("Failed to load client config from {}: {}, using defaults", path,
             e.what());
    return Default();
  }
}

} // namespace anycache
