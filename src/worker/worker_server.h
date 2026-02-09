#pragma once

#include "common/config.h"
#include "common/metrics_server.h"
#include "common/status.h"
#include "worker/block_store.h"
#include "worker/data_mover.h"
#include "worker/master_client.h"
#include "worker/page_store.h"
#include "worker/worker_service_impl.h"

#include <atomic>
#include <memory>
#include <thread>

#include <grpcpp/grpcpp.h>

namespace anycache {

// WorkerServer hosts the BlockStore/PageStore and exposes gRPC worker services.
// It registers with the Master and sends periodic heartbeats.
class WorkerServer {
public:
  explicit WorkerServer(const WorkerConfig &config,
                        const RpcConfig &rpc_config = {});
  explicit WorkerServer(const Config &config);
  ~WorkerServer();

  Status Start();
  void Wait();
  void Stop();

  // Direct in-process API (for testing)
  BlockStore &GetBlockStore() { return *block_store_; }
  PageStore &GetPageStore() { return *page_store_; }

  bool IsRunning() const { return running_; }
  WorkerId GetWorkerId() const { return worker_id_; }

private:
  void HeartbeatLoop();
  uint64_t GetTotalCapacity() const;
  uint64_t GetTotalUsed() const;

  WorkerConfig config_;
  RpcConfig rpc_config_;
  Config full_config_; // For UFS creation in CacheBlock; empty when
                       // ctor(WorkerConfig)
  std::unique_ptr<BlockStore> block_store_;
  std::unique_ptr<PageStore> page_store_;
  std::unique_ptr<DataMover> data_mover_;
  std::unique_ptr<WorkerServiceImpl> service_impl_;
  std::unique_ptr<grpc::Server> grpc_server_;
  std::unique_ptr<MasterClient> master_client_;

  WorkerId worker_id_ = kInvalidWorkerId;
  std::atomic<bool> running_{false};
  std::thread heartbeat_thread_;
  std::unique_ptr<MetricsHttpServer> metrics_server_;
};

} // namespace anycache
