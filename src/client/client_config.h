#pragma once

#include "common/types.h"

#include <chrono>
#include <string>

namespace anycache {

// Client-only configuration. Load from a dedicated config file so that
// the client library can be used without the full server config.
//
// Compatible with existing YAML: can read from the same file as master/worker
// by using the "client" section or top-level fallbacks.
struct ClientConfig {
  std::string master_address = "localhost:19999";
  int master_rpc_timeout_ms = 10000; // Client -> Master; 0 = no deadline
  int worker_rpc_timeout_ms = 30000; // Client -> Worker; 0 = no deadline

  std::chrono::milliseconds MasterTimeout() const {
    return std::chrono::milliseconds(master_rpc_timeout_ms);
  }
  std::chrono::milliseconds WorkerTimeout() const {
    return std::chrono::milliseconds(worker_rpc_timeout_ms);
  }

  static ClientConfig Default();
  static ClientConfig LoadFromFile(const std::string &path);
};

} // namespace anycache
