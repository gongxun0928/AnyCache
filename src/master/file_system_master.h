#pragma once

#include "common/config.h"
#include "common/status.h"
#include "common/types.h"
#include "master/block_master.h"
#include "master/inode_store.h"
#include "master/inode_tree.h"
#include "master/worker_manager.h"

#include <memory>
#include <string>

namespace anycache {

// Forward declaration
class MountTable;

// FileSystemMaster is the top-level coordinator for Master operations.
// It combines InodeTree, BlockMaster, WorkerManager, and MountTable.
class FileSystemMaster {
public:
  explicit FileSystemMaster(const MasterConfig &config);

  // Initialize: open InodeStore (RocksDB) and recover InodeTree.
  // Must be called before any file operations.
  Status Init();

  // ─── File operations ─────────────────────────────────────
  Status GetFileInfo(const std::string &path, Inode *out);
  Status CreateFile(const std::string &path, uint32_t mode, InodeId *out_id,
                    WorkerId *out_worker_id);
  Status CompleteFile(InodeId file_id, uint64_t size);
  Status DeleteFile(const std::string &path, bool recursive);
  Status RenameFile(const std::string &src, const std::string &dst);
  Status ListStatus(const std::string &path, std::vector<Inode> *entries);
  Status Mkdir(const std::string &path, uint32_t mode, bool recursive);
  Status TruncateFile(const std::string &path, uint64_t new_size);

  // ─── Block operations ────────────────────────────────────
  Status GetBlockLocations(const std::vector<BlockId> &block_ids,
                           std::vector<BlockLocationInfo> *locations);
  void ReportBlockLocation(BlockId block_id, WorkerId worker_id,
                           const std::string &address, TierType tier);

  // ─── Worker management ───────────────────────────────────
  Status RegisterWorker(const std::string &address, uint64_t capacity,
                        uint64_t used, WorkerId *out_id);
  Status WorkerHeartbeat(WorkerId id, uint64_t capacity, uint64_t used);

  // ─── Getters ─────────────────────────────────────────────
  InodeTree &GetInodeTree() { return inode_tree_; }
  BlockMaster &GetBlockMaster() { return block_master_; }
  WorkerManager &GetWorkerManager() { return worker_mgr_; }

private:
  MasterConfig config_;
  std::unique_ptr<InodeStore> inode_store_;
  InodeTree inode_tree_;
  BlockMaster block_master_;
  WorkerManager worker_mgr_;
};

} // namespace anycache
