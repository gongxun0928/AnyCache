#include "common/config.h"
#include "common/logging.h"
#include "master/master_server.h"

#include <csignal>
#include <iostream>

static std::atomic<bool> g_running{true};
static void SignalHandler(int sig) {
  (void)sig;
  g_running = false;
}

int main(int argc, char *argv[]) {
  anycache::Logger::Init("master");

  std::string config_path;
  if (argc > 1)
    config_path = argv[1];

  anycache::Config cfg;
  if (!config_path.empty()) {
    cfg = anycache::Config::LoadFromFile(config_path);
  } else {
    cfg = anycache::Config::Default();
  }

  anycache::MasterServer server(cfg);
  auto s = server.Start();
  if (!s.ok()) {
    LOG_CRITICAL("Failed to start master: {}", s.ToString());
    return 1;
  }

  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  LOG_INFO("AnyCache Master running. Press Ctrl+C to stop.");
  while (g_running) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  server.Stop();
  LOG_INFO("AnyCache Master exited.");
  return 0;
}
