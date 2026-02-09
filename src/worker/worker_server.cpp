#include "worker/worker_server.h"
#include "common/logging.h"

namespace anycache {

WorkerServer::WorkerServer(const Config &config)
    : config_(config.worker), rpc_config_(config.rpc), full_config_(config) {
  BlockStore::Options opts;
  opts.tiers = config_.tiers;
  opts.meta_db_path = "/tmp/anycache/worker_meta";
  opts.cache_policy = CacheManager::PolicyType::kLRU;
  block_store_ = std::make_unique<BlockStore>(opts);

  size_t max_pages = 0;
  for (const auto &tc : config_.tiers) {
    max_pages += tc.capacity_bytes / config_.page_size;
  }
  if (max_pages == 0)
    max_pages = 1024;
  page_store_ = std::make_unique<PageStore>(config_.page_size, max_pages);
}

WorkerServer::WorkerServer(const WorkerConfig &config,
                           const RpcConfig &rpc_config)
    : config_(config), rpc_config_(rpc_config),
      full_config_(Config::Default()) {
  full_config_.worker = config;
  BlockStore::Options opts;
  opts.tiers = config.tiers;
  opts.meta_db_path = "/tmp/anycache/worker_meta";
  opts.cache_policy = CacheManager::PolicyType::kLRU;
  block_store_ = std::make_unique<BlockStore>(opts);

  // Build PageStore
  size_t max_pages = 0;
  for (auto &tc : config.tiers) {
    max_pages += tc.capacity_bytes / config.page_size;
  }
  if (max_pages == 0)
    max_pages = 1024;
  page_store_ = std::make_unique<PageStore>(config.page_size, max_pages);
}

WorkerServer::~WorkerServer() { Stop(); }

Status WorkerServer::Start() {
  if (running_)
    return Status::AlreadyExists("worker already running");

  running_ = true;
  LOG_INFO("WorkerServer starting on {}:{}", config_.host, config_.port);

  // Recover block index from RocksDB
  auto s = block_store_->Recover();
  if (!s.ok()) {
    LOG_WARN("Block store recovery: {}", s.ToString());
  }

  // 1. Register with Master first (so CacheBlock can ReportBlockLocation)
  if (!config_.master_address.empty()) {
    master_client_ = std::make_unique<MasterClient>(
        config_.master_address,
        std::chrono::milliseconds(rpc_config_.internal_rpc_timeout_ms));

    std::string self_address =
        config_.host + ":" + std::to_string(config_.port);
    if (config_.host == "0.0.0.0") {
      self_address = "localhost:" + std::to_string(config_.port);
    }

    uint64_t capacity = GetTotalCapacity();
    uint64_t used = GetTotalUsed();

    auto reg_status = master_client_->RegisterWorker(self_address, capacity,
                                                     used, &worker_id_);
    if (reg_status.ok()) {
      LOG_INFO("Registered with Master as worker_id={}", worker_id_);
    } else {
      LOG_WARN("Failed to register with Master: {}", reg_status.ToString());
    }

    heartbeat_thread_ = std::thread(&WorkerServer::HeartbeatLoop, this);
  }

  // 2. Create DataMover for async preload/persist operations
  data_mover_ = std::make_unique<DataMover>(block_store_.get());

  // 3. Start gRPC server
  std::string address = config_.host + ":" + std::to_string(config_.port);
  service_impl_ = std::make_unique<WorkerServiceImpl>(
      block_store_.get(), page_store_.get(), &full_config_,
      master_client_.get(), worker_id_);
  service_impl_->SetDataMover(data_mover_.get());

  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  builder.RegisterService(service_impl_.get());
  grpc_server_ = builder.BuildAndStart();

  if (!grpc_server_) {
    running_ = false;
    return Status::Internal("failed to start gRPC server on " + address);
  }

  LOG_INFO("WorkerServer gRPC listening on {}", address);

  // Start Prometheus metrics HTTP server
  if (config_.metrics_port > 0) {
    metrics_server_ = std::make_unique<MetricsHttpServer>(
        static_cast<uint16_t>(config_.metrics_port));
    metrics_server_->Start();
  }

  return Status::OK();
}

void WorkerServer::Wait() {
  if (grpc_server_) {
    grpc_server_->Wait();
  }
}

void WorkerServer::Stop() {
  if (!running_)
    return;
  running_ = false;

  if (metrics_server_) {
    metrics_server_->Stop();
  }

  if (data_mover_) {
    data_mover_->Stop();
  }

  if (grpc_server_) {
    grpc_server_->Shutdown();
  }

  if (heartbeat_thread_.joinable()) {
    heartbeat_thread_.join();
  }

  LOG_INFO("WorkerServer stopped");
}

void WorkerServer::HeartbeatLoop() {
  while (running_) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    if (!running_)
      break;

    if (!master_client_)
      continue;

    uint64_t capacity = GetTotalCapacity();
    uint64_t used = GetTotalUsed();

    // If we haven't registered yet, try again
    if (worker_id_ == kInvalidWorkerId) {
      std::string self_address =
          config_.host + ":" + std::to_string(config_.port);
      if (config_.host == "0.0.0.0") {
        self_address = "localhost:" + std::to_string(config_.port);
      }
      auto s = master_client_->RegisterWorker(self_address, capacity, used,
                                              &worker_id_);
      if (s.ok()) {
        LOG_INFO("Late-registered with Master as worker_id={}", worker_id_);
      }
      continue;
    }

    auto s = master_client_->Heartbeat(worker_id_, capacity, used);
    if (!s.ok()) {
      LOG_WARN("Heartbeat failed: {}", s.ToString());
    }
  }
}

uint64_t WorkerServer::GetTotalCapacity() const {
  uint64_t total = 0;
  for (auto tier_type : {TierType::kMemory, TierType::kSSD, TierType::kHDD}) {
    total += block_store_->GetTierCapacity(tier_type);
  }
  return total;
}

uint64_t WorkerServer::GetTotalUsed() const {
  return block_store_->GetTotalCachedBytes();
}

} // namespace anycache
