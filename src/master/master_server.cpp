#include "master/master_server.h"
#include "common/logging.h"

namespace anycache {

MasterServer::MasterServer(const MasterConfig &config)
    : config_(config), fs_master_(std::make_unique<FileSystemMaster>(config)),
      mount_table_(std::make_unique<MountTable>(Config::Default())) {}

MasterServer::MasterServer(const Config &config)
    : config_(config.master),
      fs_master_(std::make_unique<FileSystemMaster>(config.master)),
      mount_table_(std::make_unique<MountTable>(config)) {}

MasterServer::~MasterServer() { Stop(); }

Status MasterServer::Start() {
  if (running_)
    return Status::AlreadyExists("server already running");

  // Initialize InodeStore (RocksDB) and recover InodeTree
  RETURN_IF_ERROR(fs_master_->Init());

  // Initialize MountTable persistence and reload saved mount points
  RETURN_IF_ERROR(mount_table_->Init(config_.meta_db_dir + "/mount_table"));

  running_ = true;

  // Start heartbeat checker thread
  heartbeat_thread_ = std::thread(&MasterServer::HeartbeatCheckLoop, this);

  // Build and start gRPC server
  std::string address = config_.host + ":" + std::to_string(config_.port);
  service_impl_ =
      std::make_unique<MasterServiceImpl>(fs_master_.get(), mount_table_.get());

  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(service_impl_.get());
  grpc_server_ = builder.BuildAndStart();

  if (!grpc_server_) {
    running_ = false;
    return Status::Internal("failed to start gRPC server on " + address);
  }

  LOG_INFO("MasterServer gRPC listening on {}", address);

  // Start Prometheus metrics HTTP server
  if (config_.metrics_port > 0) {
    metrics_server_ = std::make_unique<MetricsHttpServer>(
        static_cast<uint16_t>(config_.metrics_port));
    metrics_server_->Start();
  }

  return Status::OK();
}

void MasterServer::Wait() {
  if (grpc_server_) {
    grpc_server_->Wait();
  }
}

void MasterServer::Stop() {
  if (!running_)
    return;
  running_ = false;

  if (metrics_server_) {
    metrics_server_->Stop();
  }

  if (grpc_server_) {
    grpc_server_->Shutdown();
  }

  if (heartbeat_thread_.joinable()) {
    heartbeat_thread_.join();
  }

  LOG_INFO("MasterServer stopped");
}

void MasterServer::HeartbeatCheckLoop() {
  while (running_) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (!running_)
      break;

    auto dead = fs_master_->GetWorkerManager().CheckHeartbeats();
    for (auto wid : dead) {
      fs_master_->GetBlockMaster().RemoveWorkerBlocks(wid);
      LOG_WARN("Worker {} removed due to heartbeat timeout", wid);
    }
  }
}

} // namespace anycache
