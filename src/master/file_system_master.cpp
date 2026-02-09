#include "master/file_system_master.h"
#include "common/logging.h"
#include "common/metrics.h"

namespace anycache {

FileSystemMaster::FileSystemMaster(const MasterConfig &config)
    : config_(config), worker_mgr_(config.worker_heartbeat_timeout_ms) {
  LOG_INFO("FileSystemMaster initialized, journal_dir={}", config_.journal_dir);
}

Status FileSystemMaster::Init() {
  // ① Open InodeStore (RocksDB)
  inode_store_ = std::make_unique<InodeStore>();
  RETURN_IF_ERROR(inode_store_->Open(config_.meta_db_dir));
  LOG_INFO("InodeStore opened at {}", config_.meta_db_dir);

  // ② Inject store into InodeTree and recover
  inode_tree_.SetStore(inode_store_.get());
  RETURN_IF_ERROR(inode_tree_.Recover());
  LOG_INFO("InodeTree recovered, dir_count={}", inode_tree_.DirCount());

  return Status::OK();
}

// ─── File operations ─────────────────────────────────────────

Status FileSystemMaster::GetFileInfo(const std::string &path, Inode *out) {
  Metrics::Instance().IncrCounter("master.get_file_info");
  return inode_tree_.GetInodeByPath(path, out);
}

Status FileSystemMaster::CreateFile(const std::string &path, uint32_t mode,
                                    InodeId *out_id, WorkerId *out_worker_id) {
  Metrics::Instance().IncrCounter("master.create_file");

  RETURN_IF_ERROR(inode_tree_.CreateFile(path, mode, out_id));

  // Select a worker for writing
  auto s = worker_mgr_.SelectWorkerForWrite(out_worker_id);
  if (!s.ok()) {
    *out_worker_id = kInvalidWorkerId;
    // Still succeed; client can write later when a worker is available
  }
  return Status::OK();
}

Status FileSystemMaster::CompleteFile(InodeId file_id, uint64_t size) {
  Metrics::Instance().IncrCounter("master.complete_file");
  return inode_tree_.CompleteFile(file_id, size);
}

Status FileSystemMaster::DeleteFile(const std::string &path, bool recursive) {
  Metrics::Instance().IncrCounter("master.delete_file");

  // Get inode first to clean up blocks (derived from composite BlockId)
  Inode inode;
  auto s = inode_tree_.GetInodeByPath(path, &inode);
  if (s.ok() && !inode.is_directory) {
    uint32_t block_count = GetBlockCount(inode.size, inode.block_size);
    for (uint32_t i = 0; i < block_count; ++i) {
      block_master_.RemoveBlock(MakeBlockId(inode.id, i));
    }
  }
  return inode_tree_.Delete(path, recursive);
}

Status FileSystemMaster::RenameFile(const std::string &src,
                                    const std::string &dst) {
  Metrics::Instance().IncrCounter("master.rename_file");
  return inode_tree_.Rename(src, dst);
}

Status FileSystemMaster::ListStatus(const std::string &path,
                                    std::vector<Inode> *entries) {
  Metrics::Instance().IncrCounter("master.list_status");
  return inode_tree_.ListDirectory(path, entries);
}

Status FileSystemMaster::Mkdir(const std::string &path, uint32_t mode,
                               bool recursive) {
  Metrics::Instance().IncrCounter("master.mkdir");
  InodeId id;
  auto s = inode_tree_.CreateDirectory(path, mode, recursive, &id);
  if (s.IsAlreadyExists())
    return Status::OK(); // Idempotent mkdir
  return s;
}

Status FileSystemMaster::TruncateFile(const std::string &path,
                                      uint64_t new_size) {
  Metrics::Instance().IncrCounter("master.truncate_file");

  Inode inode;
  RETURN_IF_ERROR(inode_tree_.GetInodeByPath(path, &inode));
  if (inode.is_directory) {
    return Status::InvalidArgument("cannot truncate a directory");
  }

  // If shrinking, remove blocks beyond the new size
  if (new_size < inode.size) {
    uint32_t new_block_count = GetBlockCount(new_size, inode.block_size);
    uint32_t old_block_count = GetBlockCount(inode.size, inode.block_size);
    for (uint32_t i = new_block_count; i < old_block_count; ++i) {
      block_master_.RemoveBlock(MakeBlockId(inode.id, i));
    }
  }

  // Update size in InodeTree
  return inode_tree_.UpdateSize(inode.id, new_size);
}

// ─── Block operations ────────────────────────────────────────

Status
FileSystemMaster::GetBlockLocations(const std::vector<BlockId> &block_ids,
                                    std::vector<BlockLocationInfo> *locations) {
  return block_master_.GetBlockLocations(block_ids, locations);
}

void FileSystemMaster::ReportBlockLocation(BlockId block_id, WorkerId worker_id,
                                           const std::string &address,
                                           TierType tier) {
  block_master_.AddBlockLocation(block_id, worker_id, address, tier);
}

// ─── Worker management ───────────────────────────────────────

Status FileSystemMaster::RegisterWorker(const std::string &address,
                                        uint64_t capacity, uint64_t used,
                                        WorkerId *out_id) {
  return worker_mgr_.RegisterWorker(address, capacity, used, out_id);
}

Status FileSystemMaster::WorkerHeartbeat(WorkerId id, uint64_t capacity,
                                         uint64_t used) {
  return worker_mgr_.Heartbeat(id, capacity, used);
}

} // namespace anycache
