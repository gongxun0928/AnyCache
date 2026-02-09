#pragma once

#include "common/config.h"
#include "common/metrics_server.h"
#include "master/file_system_master.h"
#include "master/master_service_impl.h"
#include "master/mount_table.h"

#include <atomic>
#include <memory>
#include <thread>

#include <grpcpp/grpcpp.h>

namespace anycache {

// MasterServer hosts the FileSystemMaster and exposes gRPC services.
class MasterServer {
public:
  explicit MasterServer(const MasterConfig &config);
  explicit MasterServer(const Config &config);
  ~MasterServer();

  // Start the gRPC server (non-blocking)
  Status Start();

  // Block until the server shuts down
  void Wait();

  // Stop the server
  void Stop();

  // Direct access (for testing within the same process)
  FileSystemMaster &GetFileSystemMaster() { return *fs_master_; }
  MountTable &GetMountTable() { return *mount_table_; }

  bool IsRunning() const { return running_; }

private:
  void HeartbeatCheckLoop();

  MasterConfig config_;
  std::unique_ptr<FileSystemMaster> fs_master_;
  std::unique_ptr<MountTable> mount_table_;
  std::unique_ptr<MasterServiceImpl> service_impl_;
  std::unique_ptr<grpc::Server> grpc_server_;
  std::atomic<bool> running_{false};
  std::thread heartbeat_thread_;
  std::unique_ptr<MetricsHttpServer> metrics_server_;
};

} // namespace anycache
