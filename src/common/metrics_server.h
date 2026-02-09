#pragma once

#include "common/metrics.h"

#include <atomic>
#include <string>
#include <thread>

namespace anycache {

// Minimal HTTP server that exposes Prometheus-compatible /metrics endpoint.
// Uses raw POSIX sockets — no external HTTP library dependency.
//
// Usage:
//   MetricsHttpServer server(9090);
//   server.Start();     // non-blocking
//   ...
//   server.Stop();
class MetricsHttpServer {
public:
  explicit MetricsHttpServer(uint16_t port);
  ~MetricsHttpServer();

  // Start listening in a background thread (non-blocking).
  void Start();

  // Stop the server and join the background thread.
  void Stop();

  bool IsRunning() const { return running_; }
  uint16_t GetPort() const { return port_; }

private:
  void ServeLoop();

  uint16_t port_;
  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

} // namespace anycache
