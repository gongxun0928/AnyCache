#include "client/file_system_client.h"
#include "client/block_client.h"
#include "client/client_proto_utils.h"
#include "common/logging.h"

namespace anycache {

// ─── Constructors ────────────────────────────────────────────────

FileSystemClient::FileSystemClient(const std::string &master_address)
    : FileSystemClient(master_address, std::make_shared<ChannelPool>(),
                       ClientConfig::Default()) {}

FileSystemClient::FileSystemClient(const ClientConfig &config)
    : FileSystemClient(config.master_address, std::make_shared<ChannelPool>(),
                       config) {}

FileSystemClient::FileSystemClient(const std::string &master_address,
                                   std::shared_ptr<ChannelPool> channel_pool,
                                   const ClientConfig &config)
    : channel_pool_(std::move(channel_pool)),
      channel_(channel_pool_->GetChannel(master_address)),
      stub_(proto::MasterService::NewStub(channel_)),
      master_timeout_(config.MasterTimeout()),
      worker_timeout_(config.WorkerTimeout()) {
  LOG_INFO("FileSystemClient connecting to {} (master_timeout={}ms, "
           "worker_timeout={}ms)",
           master_address, config.master_rpc_timeout_ms,
           config.worker_rpc_timeout_ms);
}

FileSystemClient::~FileSystemClient() = default;

// ─── Deadline helpers ────────────────────────────────────────────

void FileSystemClient::SetMasterDeadline(grpc::ClientContext &ctx) const {
  if (master_timeout_.count() > 0) {
    ctx.set_deadline(std::chrono::system_clock::now() + master_timeout_);
  }
}

void FileSystemClient::SetWorkerDeadline(grpc::ClientContext &ctx) const {
  if (worker_timeout_.count() > 0) {
    ctx.set_deadline(std::chrono::system_clock::now() + worker_timeout_);
  }
}

// ─── File operations ─────────────────────────────────────────────

Status FileSystemClient::GetFileInfo(const std::string &path,
                                     ClientFileInfo *info) {
  proto::GetFileInfoRequest req;
  req.set_path(path);
  proto::GetFileInfoResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->GetFileInfo(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  RETURN_IF_ERROR(FromProtoStatus(resp.status()));

  info->inode_id = resp.file_info().inode_id();
  info->name = resp.file_info().name();
  info->path = resp.file_info().path();
  info->is_directory = resp.file_info().is_directory();
  info->size = resp.file_info().size();
  info->mode = resp.file_info().mode();
  info->modification_time_ms = resp.file_info().modification_time_ms();
  return Status::OK();
}

Status FileSystemClient::CreateFile(const std::string &path, uint32_t mode) {
  InodeId id;
  WorkerId wid;
  return CreateFileEx(path, mode, &id, &wid);
}

Status FileSystemClient::CreateFileEx(const std::string &path, uint32_t mode,
                                      InodeId *out_id, WorkerId *out_worker_id,
                                      std::string *out_worker_address) {
  proto::CreateFileRequest req;
  req.set_path(path);
  req.set_mode(mode);
  proto::CreateFileResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->CreateFile(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  RETURN_IF_ERROR(FromProtoStatus(resp.status()));

  *out_id = resp.file_id();
  *out_worker_id = resp.worker_id();
  if (out_worker_address)
    *out_worker_address = resp.worker_address();
  return Status::OK();
}

Status FileSystemClient::CompleteFile(InodeId file_id, uint64_t size) {
  proto::CompleteFileRequest req;
  req.set_file_id(file_id);
  req.set_file_size(size);
  proto::CompleteFileResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->CompleteFile(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  return FromProtoStatus(resp.status());
}

Status FileSystemClient::DeleteFile(const std::string &path, bool recursive) {
  proto::DeleteFileRequest req;
  req.set_path(path);
  req.set_recursive(recursive);
  proto::DeleteFileResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->DeleteFile(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  return FromProtoStatus(resp.status());
}

Status FileSystemClient::RenameFile(const std::string &src,
                                    const std::string &dst) {
  proto::RenameFileRequest req;
  req.set_src_path(src);
  req.set_dst_path(dst);
  proto::RenameFileResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->RenameFile(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  return FromProtoStatus(resp.status());
}

Status FileSystemClient::ListStatus(const std::string &path,
                                    std::vector<ClientFileInfo> *entries) {
  proto::ListStatusRequest req;
  req.set_path(path);
  proto::ListStatusResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->ListStatus(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  RETURN_IF_ERROR(FromProtoStatus(resp.status()));

  for (const auto &fi : resp.entries()) {
    ClientFileInfo info;
    info.inode_id = fi.inode_id();
    info.name = fi.name();
    info.path = fi.path();
    info.is_directory = fi.is_directory();
    info.size = fi.size();
    info.mode = fi.mode();
    info.modification_time_ms = fi.modification_time_ms();
    entries->push_back(std::move(info));
  }
  return Status::OK();
}

Status FileSystemClient::Mkdir(const std::string &path, bool recursive) {
  proto::MkdirRequest req;
  req.set_path(path);
  req.set_recursive(recursive);
  req.set_mode(0755);
  proto::MkdirResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->Mkdir(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  return FromProtoStatus(resp.status());
}

Status FileSystemClient::TruncateFile(const std::string &path,
                                      uint64_t new_size) {
  proto::TruncateFileRequest req;
  req.set_path(path);
  req.set_new_size(new_size);
  proto::TruncateFileResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->TruncateFile(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  return FromProtoStatus(resp.status());
}

// ─── Mount operations ────────────────────────────────────────────

Status FileSystemClient::Mount(const std::string &anycache_path,
                               const std::string &ufs_uri) {
  proto::MountRequest req;
  req.set_anycache_path(anycache_path);
  req.set_ufs_uri(ufs_uri);
  proto::MountResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->Mount(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  return FromProtoStatus(resp.status());
}

Status FileSystemClient::Unmount(const std::string &anycache_path) {
  proto::UnmountRequest req;
  req.set_anycache_path(anycache_path);
  proto::UnmountResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->Unmount(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  return FromProtoStatus(resp.status());
}

Status FileSystemClient::GetMountTable(
    std::vector<std::pair<std::string, std::string>> *mounts) {
  proto::GetMountTableRequest req;
  proto::GetMountTableResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->GetMountTable(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  RETURN_IF_ERROR(FromProtoStatus(resp.status()));

  for (const auto &[path, uri] : resp.mounts()) {
    mounts->emplace_back(path, uri);
  }
  return Status::OK();
}

// ─── Block operations ────────────────────────────────────────────

Status FileSystemClient::GetBlockLocations(
    const std::vector<BlockId> &block_ids,
    std::vector<ClientBlockLocation> *locations) {
  proto::GetBlockLocationsRequest req;
  for (auto id : block_ids) {
    req.add_block_ids(id);
  }
  proto::GetBlockLocationsResponse resp;
  grpc::ClientContext ctx;
  SetMasterDeadline(ctx);

  auto grpc_status = stub_->GetBlockLocations(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  RETURN_IF_ERROR(FromProtoStatus(resp.status()));

  for (const auto &bl : resp.locations()) {
    ClientBlockLocation loc;
    loc.block_id = bl.block_id();
    loc.worker_id = bl.worker_id();
    loc.worker_address = bl.worker_address();
    loc.tier = FromProtoTier(bl.tier());
    locations->push_back(std::move(loc));
  }
  return Status::OK();
}

// ─── Read/Write convenience ──────────────────────────────────────

Status FileSystemClient::ReadFile(const std::string &path, void *buf,
                                  size_t size, off_t offset,
                                  size_t *bytes_read) {
  // 1. Get file info to determine block layout
  ClientFileInfo file_info;
  RETURN_IF_ERROR(GetFileInfo(path, &file_info));

  if (static_cast<uint64_t>(offset) >= file_info.size) {
    *bytes_read = 0;
    return Status::OK();
  }

  size_t readable =
      std::min(size, static_cast<size_t>(file_info.size - offset));
  size_t block_size = kDefaultBlockSize;

  size_t total_read = 0;
  while (total_read < readable) {
    uint64_t abs_offset = offset + total_read;
    uint32_t block_idx = static_cast<uint32_t>(abs_offset / block_size);
    size_t block_offset = abs_offset % block_size;
    size_t to_read = std::min(readable - total_read, block_size - block_offset);

    BlockId bid = MakeBlockId(file_info.inode_id, block_idx);

    // 2. Get block location from Master
    std::vector<ClientBlockLocation> locations;
    RETURN_IF_ERROR(GetBlockLocations({bid}, &locations));
    if (locations.empty()) {
      break; // No worker has this block
    }

    // 3. Read from the first available worker (Channel from pool)
    auto worker_channel =
        channel_pool_->GetChannel(locations[0].worker_address);
    BlockClient block_client(worker_channel, worker_timeout_);
    auto s = block_client.ReadBlock(bid, static_cast<char *>(buf) + total_read,
                                    to_read, static_cast<off_t>(block_offset));
    if (!s.ok())
      break;

    total_read += to_read;
  }

  *bytes_read = total_read;
  return Status::OK();
}

Status FileSystemClient::WriteFile(const std::string &path, const void *buf,
                                   size_t size, off_t offset,
                                   size_t *bytes_written) {
  // 1. Get or create file
  ClientFileInfo file_info;
  std::string assigned_worker_address; // From CreateFileEx for new file
  auto s = GetFileInfo(path, &file_info);
  if (!s.ok()) {
    // File doesn't exist, create it
    InodeId id;
    WorkerId wid;
    RETURN_IF_ERROR(
        CreateFileEx(path, 0644, &id, &wid, &assigned_worker_address));
    file_info.inode_id = id;
    file_info.size = 0;
  }

  size_t block_size = kDefaultBlockSize;
  size_t total_written = 0;

  while (total_written < size) {
    uint64_t abs_offset = static_cast<uint64_t>(offset) + total_written;
    uint32_t block_idx = static_cast<uint32_t>(abs_offset / block_size);
    size_t offset_in_block = abs_offset % block_size;
    size_t to_write =
        std::min(size - total_written, block_size - offset_in_block);

    BlockId block_id = MakeBlockId(file_info.inode_id, block_idx);

    // Get block location (or use assigned worker for new blocks)
    std::vector<ClientBlockLocation> locations;
    RETURN_IF_ERROR(GetBlockLocations({block_id}, &locations));

    std::string worker_address;
    if (!locations.empty()) {
      worker_address = locations[0].worker_address;
    } else if (!assigned_worker_address.empty()) {
      // New block: use worker from CreateFileEx
      worker_address = assigned_worker_address;
    } else if (block_idx > 0) {
      // Extending existing file: try block 0's worker for locality
      BlockId block0 = MakeBlockId(file_info.inode_id, 0);
      std::vector<ClientBlockLocation> loc0;
      RETURN_IF_ERROR(GetBlockLocations({block0}, &loc0));
      if (!loc0.empty()) {
        worker_address = loc0[0].worker_address;
      }
    }
    if (worker_address.empty()) {
      *bytes_written = total_written;
      return Status::Unavailable("no worker available for block");
    }

    // Write to worker (Channel from pool)
    auto worker_channel = channel_pool_->GetChannel(worker_address);
    BlockClient block_client(worker_channel, worker_timeout_);
    auto ws = block_client.WriteBlock(
        block_id, static_cast<const char *>(buf) + total_written, to_write,
        static_cast<off_t>(offset_in_block));
    if (!ws.ok()) {
      break;
    }

    total_written += to_write;
  }

  *bytes_written = total_written;
  return Status::OK();
}

} // namespace anycache
