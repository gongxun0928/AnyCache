#pragma once

#include "common/status.h"
#include "common/types.h"
#include "ufs/ufs.h"
#include "worker/block_store.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace anycache {

// DataMover handles preloading (UFS -> cache) and persisting (cache -> UFS).
//
// Supports two modes:
//   1. Constructor UFS: a fixed UFS instance shared by all tasks (legacy).
//   2. Per-task UFS: each task carries its own UFS (for async RPC operations
//      where the UFS is determined by the request's ufs_path).
class DataMover {
public:
  struct Task {
    enum Type { kPreload, kPersist };
    Type type;
    BlockId block_id; // composite BlockId (preload target / persist source)
    std::string ufs_path;
    uint64_t offset_in_ufs;
    uint64_t length;
    std::shared_ptr<UnderFileSystem> owned_ufs; // per-task UFS (optional)
  };

  DataMover(BlockStore *block_store, UnderFileSystem *ufs, int num_threads = 2);

  // Construct without a default UFS; all tasks must supply their own.
  DataMover(BlockStore *block_store, int num_threads = 2);

  ~DataMover();

  // Schedule a preload: read from UFS into cache block
  Status SubmitPreload(BlockId block_id, const std::string &ufs_path,
                       uint64_t offset, uint64_t length);

  // Schedule a preload with a per-task UFS
  Status SubmitPreload(BlockId block_id, const std::string &ufs_path,
                       uint64_t offset, uint64_t length,
                       std::shared_ptr<UnderFileSystem> ufs);

  // Schedule a persist: write from cache to UFS
  Status SubmitPersist(BlockId block_id, const std::string &ufs_path,
                       uint64_t offset);

  // Schedule a persist with a per-task UFS
  Status SubmitPersist(BlockId block_id, const std::string &ufs_path,
                       uint64_t offset, std::shared_ptr<UnderFileSystem> ufs);

  // Wait for all pending tasks to complete
  void WaitAll();

  // Stop background threads
  void Stop();

  size_t GetPendingTaskCount() const;

private:
  void WorkerLoop();
  Status ExecuteTask(const Task &task);

  BlockStore *block_store_;
  UnderFileSystem *ufs_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::queue<Task> tasks_;
  std::vector<std::thread> threads_;
  std::atomic<bool> running_{true};
  std::atomic<size_t> active_tasks_{0};
  std::condition_variable done_cv_;
};

} // namespace anycache
