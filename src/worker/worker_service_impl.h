#pragma once

#include "common/config.h"
#include "worker/block_store.h"
#include "worker/data_mover.h"
#include "worker/master_client.h"
#include "worker/page_store.h"

#include "worker.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace anycache {

// WorkerServiceImpl bridges gRPC requests to the BlockStore and PageStore.
class WorkerServiceImpl final : public proto::WorkerService::Service {
public:
  WorkerServiceImpl(BlockStore *block_store, PageStore *page_store);

  WorkerServiceImpl(BlockStore *block_store, PageStore *page_store,
                    const Config *config, MasterClient *master_client,
                    WorkerId worker_id);

  // Set the DataMover for async operations (CacheBlock / PersistBlock)
  void SetDataMover(DataMover *data_mover) { data_mover_ = data_mover; }

  // ─── Block I/O ───────────────────────────────────────────
  grpc::Status ReadBlock(grpc::ServerContext *ctx,
                         const proto::ReadBlockRequest *req,
                         proto::ReadBlockResponse *resp) override;

  grpc::Status WriteBlock(grpc::ServerContext *ctx,
                          const proto::WriteBlockRequest *req,
                          proto::WriteBlockResponse *resp) override;

  grpc::Status CacheBlock(grpc::ServerContext *ctx,
                          const proto::CacheBlockRequest *req,
                          proto::CacheBlockResponse *resp) override;

  grpc::Status RemoveBlock(grpc::ServerContext *ctx,
                           const proto::RemoveBlockRequest *req,
                           proto::RemoveBlockResponse *resp) override;

  // ─── Page I/O ────────────────────────────────────────────
  grpc::Status ReadPage(grpc::ServerContext *ctx,
                        const proto::ReadPageRequest *req,
                        proto::ReadPageResponse *resp) override;

  // ─── Async operations ────────────────────────────────────
  grpc::Status AsyncCacheBlock(grpc::ServerContext *ctx,
                               const proto::AsyncCacheBlockRequest *req,
                               proto::AsyncCacheBlockResponse *resp) override;

  grpc::Status PersistBlock(grpc::ServerContext *ctx,
                            const proto::PersistBlockRequest *req,
                            proto::PersistBlockResponse *resp) override;

  // ─── Status ──────────────────────────────────────────────
  grpc::Status GetWorkerStatus(grpc::ServerContext *ctx,
                               const proto::GetWorkerStatusRequest *req,
                               proto::GetWorkerStatusResponse *resp) override;

private:
  // Build worker self-address from config (used for ReportBlockLocation)
  std::string GetSelfAddress() const;

  BlockStore *block_store_;
  PageStore *page_store_;
  const Config *config_ = nullptr; // For UFS creation in CacheBlock
  MasterClient *master_client_ = nullptr;
  DataMover *data_mover_ = nullptr; // For async cache/persist operations
  WorkerId worker_id_ = kInvalidWorkerId;
};

} // namespace anycache
