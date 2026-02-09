#include "master/master_service_impl.h"
#include "common/logging.h"
#include "common/proto_utils.h"

namespace anycache {

MasterServiceImpl::MasterServiceImpl(FileSystemMaster *master,
                                     MountTable *mount_table)
    : master_(master), mount_table_(mount_table) {}

// ─── File operations ─────────────────────────────────────────────

grpc::Status
MasterServiceImpl::GetFileInfo(grpc::ServerContext * /*ctx*/,
                               const proto::GetFileInfoRequest *req,
                               proto::GetFileInfoResponse *resp) {
  Inode inode;
  auto s = master_->GetFileInfo(req->path(), &inode);
  *resp->mutable_status() = ToProtoStatus(s);
  if (s.ok()) {
    *resp->mutable_file_info() = InodeToProto(inode);
  }
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::CreateFile(grpc::ServerContext * /*ctx*/,
                                           const proto::CreateFileRequest *req,
                                           proto::CreateFileResponse *resp) {
  InodeId file_id;
  WorkerId worker_id;
  uint32_t mode = req->mode() != 0 ? req->mode() : 0644;
  auto s = master_->CreateFile(req->path(), mode, &file_id, &worker_id);
  *resp->mutable_status() = ToProtoStatus(s);
  if (s.ok()) {
    resp->set_file_id(file_id);
    resp->set_worker_id(worker_id);
    WorkerState state;
    if (worker_id != kInvalidWorkerId &&
        master_->GetWorkerManager().GetWorker(worker_id, &state).ok()) {
      resp->set_worker_address(state.address);
    }
  }
  return grpc::Status::OK;
}

grpc::Status
MasterServiceImpl::CompleteFile(grpc::ServerContext * /*ctx*/,
                                const proto::CompleteFileRequest *req,
                                proto::CompleteFileResponse *resp) {
  auto s = master_->CompleteFile(req->file_id(), req->file_size());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::DeleteFile(grpc::ServerContext * /*ctx*/,
                                           const proto::DeleteFileRequest *req,
                                           proto::DeleteFileResponse *resp) {
  auto s = master_->DeleteFile(req->path(), req->recursive());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::RenameFile(grpc::ServerContext * /*ctx*/,
                                           const proto::RenameFileRequest *req,
                                           proto::RenameFileResponse *resp) {
  auto s = master_->RenameFile(req->src_path(), req->dst_path());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::ListStatus(grpc::ServerContext * /*ctx*/,
                                           const proto::ListStatusRequest *req,
                                           proto::ListStatusResponse *resp) {
  std::vector<Inode> inodes;
  auto s = master_->ListStatus(req->path(), &inodes);
  *resp->mutable_status() = ToProtoStatus(s);
  if (s.ok()) {
    for (auto &inode : inodes) {
      *resp->add_entries() = InodeToProto(inode);
    }
  }
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::Mkdir(grpc::ServerContext * /*ctx*/,
                                      const proto::MkdirRequest *req,
                                      proto::MkdirResponse *resp) {
  uint32_t mode = req->mode() != 0 ? req->mode() : 0755;
  auto s = master_->Mkdir(req->path(), mode, req->recursive());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

grpc::Status
MasterServiceImpl::TruncateFile(grpc::ServerContext * /*ctx*/,
                                const proto::TruncateFileRequest *req,
                                proto::TruncateFileResponse *resp) {
  auto s = master_->TruncateFile(req->path(), req->new_size());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

// ─── Block operations ────────────────────────────────────────────

grpc::Status
MasterServiceImpl::GetBlockLocations(grpc::ServerContext * /*ctx*/,
                                     const proto::GetBlockLocationsRequest *req,
                                     proto::GetBlockLocationsResponse *resp) {
  std::vector<BlockId> block_ids(req->block_ids().begin(),
                                 req->block_ids().end());
  std::vector<BlockLocationInfo> locations;
  auto s = master_->GetBlockLocations(block_ids, &locations);
  *resp->mutable_status() = ToProtoStatus(s);
  if (s.ok()) {
    for (auto &loc : locations) {
      *resp->add_locations() = BlockLocationToProto(loc);
    }
  }
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::ReportBlockLocation(
    grpc::ServerContext * /*ctx*/, const proto::ReportBlockLocationRequest *req,
    proto::ReportBlockLocationResponse *resp) {
  for (const auto &bl : req->blocks()) {
    master_->ReportBlockLocation(bl.block_id(), req->worker_id(),
                                 bl.worker_address(), FromProtoTier(bl.tier()));
  }
  *resp->mutable_status() = ToProtoStatus(Status::OK());
  return grpc::Status::OK;
}

// ─── Worker management ───────────────────────────────────────────

grpc::Status
MasterServiceImpl::RegisterWorker(grpc::ServerContext * /*ctx*/,
                                  const proto::RegisterWorkerRequest *req,
                                  proto::RegisterWorkerResponse *resp) {
  WorkerId worker_id;
  auto s = master_->RegisterWorker(req->address(), req->capacity_bytes(),
                                   req->used_bytes(), &worker_id);
  *resp->mutable_status() = ToProtoStatus(s);
  if (s.ok()) {
    resp->set_worker_id(worker_id);
  }
  return grpc::Status::OK;
}

grpc::Status
MasterServiceImpl::WorkerHeartbeat(grpc::ServerContext * /*ctx*/,
                                   const proto::WorkerHeartbeatRequest *req,
                                   proto::WorkerHeartbeatResponse *resp) {
  auto s = master_->WorkerHeartbeat(req->worker_id(), req->capacity_bytes(),
                                    req->used_bytes());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

// ─── Mount operations ────────────────────────────────────────────

grpc::Status MasterServiceImpl::Mount(grpc::ServerContext * /*ctx*/,
                                      const proto::MountRequest *req,
                                      proto::MountResponse *resp) {
  if (!mount_table_) {
    *resp->mutable_status() =
        ToProtoStatus(Status::Internal("mount table not available"));
    return grpc::Status::OK;
  }
  auto s = mount_table_->Mount(req->anycache_path(), req->ufs_uri());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::Unmount(grpc::ServerContext * /*ctx*/,
                                        const proto::UnmountRequest *req,
                                        proto::UnmountResponse *resp) {
  if (!mount_table_) {
    *resp->mutable_status() =
        ToProtoStatus(Status::Internal("mount table not available"));
    return grpc::Status::OK;
  }
  auto s = mount_table_->Unmount(req->anycache_path());
  *resp->mutable_status() = ToProtoStatus(s);
  return grpc::Status::OK;
}

grpc::Status
MasterServiceImpl::GetMountTable(grpc::ServerContext * /*ctx*/,
                                 const proto::GetMountTableRequest * /*req*/,
                                 proto::GetMountTableResponse *resp) {
  if (!mount_table_) {
    *resp->mutable_status() =
        ToProtoStatus(Status::Internal("mount table not available"));
    return grpc::Status::OK;
  }
  auto mounts = mount_table_->GetMountPoints();
  *resp->mutable_status() = ToProtoStatus(Status::OK());
  for (auto &[path, uri] : mounts) {
    (*resp->mutable_mounts())[path] = uri;
  }
  return grpc::Status::OK;
}

} // namespace anycache
