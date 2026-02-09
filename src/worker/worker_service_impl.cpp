#include "worker/worker_service_impl.h"
#include "common/logging.h"
#include "common/proto_utils.h"
#include "ufs/ufs_factory.h"

#include <fcntl.h>

namespace anycache {

WorkerServiceImpl::WorkerServiceImpl(BlockStore *block_store,
                                     PageStore *page_store)
    : block_store_(block_store), page_store_(page_store) {}

WorkerServiceImpl::WorkerServiceImpl(BlockStore *block_store,
                                     PageStore *page_store,
                                     const Config *config,
                                     MasterClient *master_client,
                                     WorkerId worker_id)
    : block_store_(block_store), page_store_(page_store), config_(config),
      master_client_(master_client), worker_id_(worker_id) {}

// ─── Block I/O ───────────────────────────────────────────────────

grpc::Status WorkerServiceImpl::ReadBlock(grpc::ServerContext * /*ctx*/,
                                          const proto::ReadBlockRequest *req,
                                          proto::ReadBlockResponse *resp) {
  size_t length = req->length();
  std::string buf(length, '\0');
  auto s = block_store_->ReadBlock(req->block_id(), buf.data(), length,
                                   static_cast<off_t>(req->offset()));
  *resp->mutable_status() = ToProtoStatus(s);
  if (s.ok()) {
    resp->set_data(std::move(buf));
  }
  return grpc::Status::OK;
}

grpc::Status WorkerServiceImpl::WriteBlock(grpc::ServerContext * /*ctx*/,
                                           const proto::WriteBlockRequest *req,
                                           proto::WriteBlockResponse *resp) {
  BlockId block_id = req->block_id();

  // Ensure the block exists before writing
  auto s = block_store_->EnsureBlock(block_id, req->data().size());
  if (!s.ok()) {
    *resp->mutable_status() = ToProtoStatus(s);
    return grpc::Status::OK;
  }

  s = block_store_->WriteBlock(block_id, req->data().data(), req->data().size(),
                               static_cast<off_t>(req->offset()));
  *resp->mutable_status() = ToProtoStatus(s);
  if (s.ok()) {
    resp->set_block_id(block_id);
    // Report block location to Master so subsequent reads can find it
    if (master_client_ && worker_id_ != kInvalidWorkerId && config_) {
      master_client_->ReportBlockLocation(block_id, worker_id_,
                                          GetSelfAddress(), TierType::kMemory);
    }
  }
  return grpc::Status::OK;
}

grpc::Status WorkerServiceImpl::CacheBlock(grpc::ServerContext * /*ctx*/,
                                           const proto::CacheBlockRequest *req,
                                           proto::CacheBlockResponse *resp) {
  if (!config_) {
    *resp->mutable_status() =
        ToProtoStatus(Status::InvalidArgument("worker not configured for UFS"));
    return grpc::Status::OK;
  }

  const std::string &ufs_path = req->ufs_path();
  if (ufs_path.empty()) {
    *resp->mutable_status() =
        ToProtoStatus(Status::InvalidArgument("ufs_path is required"));
    return grpc::Status::OK;
  }

  // Split ufs_path into base_uri and relative path
  // e.g. "file:///mnt/data/file" -> base="file:///mnt/data", rel="file"
  auto pos = ufs_path.find("://");
  std::string base_uri;
  std::string rel_path;
  if (pos == std::string::npos) {
    base_uri = "file://";
    rel_path = ufs_path;
  } else {
    base_uri = ufs_path.substr(0, pos + 3);
    std::string path_part = ufs_path.substr(pos + 3);
    auto slash = path_part.rfind('/');
    if (slash != std::string::npos) {
      base_uri += path_part.substr(0, slash);
      rel_path = path_part.substr(slash + 1);
    } else {
      base_uri += path_part;
      rel_path = "";
    }
  }

  auto ufs = UfsFactory::Create(base_uri, *config_);
  if (!ufs) {
    *resp->mutable_status() = ToProtoStatus(
        Status::InvalidArgument("failed to create UFS for " + base_uri));
    return grpc::Status::OK;
  }

  UfsFileHandle handle;
  auto s = ufs->Open(rel_path, O_RDONLY, &handle);
  if (!s.ok()) {
    *resp->mutable_status() = ToProtoStatus(s);
    return grpc::Status::OK;
  }

  std::vector<char> buf(req->length());
  size_t bytes_read = 0;
  s = ufs->Read(handle, buf.data(), req->length(),
                static_cast<off_t>(req->offset_in_ufs()), &bytes_read);
  ufs->Close(handle);
  if (!s.ok()) {
    *resp->mutable_status() = ToProtoStatus(s);
    return grpc::Status::OK;
  }

  s = block_store_->EnsureBlock(req->block_id(), bytes_read);
  if (!s.ok()) {
    *resp->mutable_status() = ToProtoStatus(s);
    return grpc::Status::OK;
  }

  s = block_store_->WriteBlock(req->block_id(), buf.data(), bytes_read, 0);
  if (!s.ok()) {
    *resp->mutable_status() = ToProtoStatus(s);
    return grpc::Status::OK;
  }

  if (master_client_ && worker_id_ != kInvalidWorkerId && config_) {
    master_client_->ReportBlockLocation(req->block_id(), worker_id_,
                                        GetSelfAddress(), TierType::kMemory);
  }

  *resp->mutable_status() = ToProtoStatus(Status::OK());
  return grpc::Status::OK;
}

grpc::Status
WorkerServiceImpl::RemoveBlock(grpc::ServerContext * /*ctx*/,
                               const proto::RemoveBlockRequest *req,
                               proto::RemoveBlockResponse *resp) {
  auto s = block_store_->RemoveBlock(req->block_id());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

// ─── Page I/O ────────────────────────────────────────────────────

grpc::Status WorkerServiceImpl::ReadPage(grpc::ServerContext * /*ctx*/,
                                         const proto::ReadPageRequest *req,
                                         proto::ReadPageResponse *resp) {
  size_t page_size = page_store_->GetPageSize();
  std::string buf(page_size, '\0');
  size_t bytes_read = 0;
  auto s = page_store_->ReadPage(req->file_id(), req->page_index(), buf.data(),
                                 &bytes_read);
  *resp->mutable_status() = ToProtoStatus(s);
  if (s.ok()) {
    buf.resize(bytes_read);
    resp->set_data(std::move(buf));
  }
  return grpc::Status::OK;
}

// ─── Async operations ────────────────────────────────────────────

grpc::Status
WorkerServiceImpl::AsyncCacheBlock(grpc::ServerContext * /*ctx*/,
                                   const proto::AsyncCacheBlockRequest *req,
                                   proto::AsyncCacheBlockResponse *resp) {
  if (!config_) {
    *resp->mutable_status() =
        ToProtoStatus(Status::InvalidArgument("worker not configured for UFS"));
    return grpc::Status::OK;
  }
  if (req->ufs_path().empty()) {
    *resp->mutable_status() =
        ToProtoStatus(Status::InvalidArgument("ufs_path is required"));
    return grpc::Status::OK;
  }
  if (!data_mover_) {
    *resp->mutable_status() = ToProtoStatus(
        Status::Internal("DataMover not available for async operations"));
    return grpc::Status::OK;
  }

  // Submit asynchronous preload task via DataMover; returns immediately.
  auto s = data_mover_->SubmitPreload(req->block_id(), req->ufs_path(),
                                      req->offset_in_ufs(), req->length());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

grpc::Status
WorkerServiceImpl::PersistBlock(grpc::ServerContext * /*ctx*/,
                                const proto::PersistBlockRequest *req,
                                proto::PersistBlockResponse *resp) {
  if (!config_) {
    *resp->mutable_status() =
        ToProtoStatus(Status::InvalidArgument("worker not configured for UFS"));
    return grpc::Status::OK;
  }
  if (req->ufs_path().empty()) {
    *resp->mutable_status() =
        ToProtoStatus(Status::InvalidArgument("ufs_path is required"));
    return grpc::Status::OK;
  }
  if (!data_mover_) {
    *resp->mutable_status() = ToProtoStatus(
        Status::Internal("DataMover not available for async operations"));
    return grpc::Status::OK;
  }

  // Submit asynchronous persist task via DataMover; returns immediately.
  auto s = data_mover_->SubmitPersist(req->block_id(), req->ufs_path(),
                                      req->offset_in_ufs());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

// ─── Status ──────────────────────────────────────────────────────

grpc::Status WorkerServiceImpl::GetWorkerStatus(
    grpc::ServerContext * /*ctx*/,
    const proto::GetWorkerStatusRequest * /*req*/,
    proto::GetWorkerStatusResponse *resp) {
  *resp->mutable_status() = ToProtoStatus(Status::OK());

  uint64_t total_capacity = 0;
  uint64_t total_used = 0;

  // Report per-tier stats
  for (auto tier_type : {TierType::kMemory, TierType::kSSD, TierType::kHDD}) {
    size_t cap = block_store_->GetTierCapacity(tier_type);
    size_t used = block_store_->GetTierUsedBytes(tier_type);
    if (cap > 0) {
      auto *ts = resp->add_tiers();
      ts->set_type(ToProtoTier(tier_type));
      ts->set_capacity_bytes(cap);
      ts->set_used_bytes(used);
      total_capacity += cap;
      total_used += used;
    }
  }

  resp->set_capacity_bytes(total_capacity);
  resp->set_used_bytes(total_used);
  // block_count is approximate: total cached bytes / default block size
  resp->set_block_count(block_store_->GetTotalCachedBytes() /
                        kDefaultBlockSize);
  return grpc::Status::OK;
}

// ─── Helpers ─────────────────────────────────────────────────────

std::string WorkerServiceImpl::GetSelfAddress() const {
  if (!config_)
    return "";
  std::string addr =
      config_->worker.host + ":" + std::to_string(config_->worker.port);
  if (config_->worker.host == "0.0.0.0") {
    addr = "localhost:" + std::to_string(config_->worker.port);
  }
  return addr;
}

} // namespace anycache
