#include "worker/data_mover.h"
#include "common/logging.h"
#include "common/metrics.h"

#include <fcntl.h>

namespace anycache {

DataMover::DataMover(BlockStore *block_store, UnderFileSystem *ufs,
                     int num_threads)
    : block_store_(block_store), ufs_(ufs) {
  for (int i = 0; i < num_threads; ++i) {
    threads_.emplace_back(&DataMover::WorkerLoop, this);
  }
  LOG_INFO("DataMover started with {} threads", num_threads);
}

DataMover::DataMover(BlockStore *block_store, int num_threads)
    : block_store_(block_store), ufs_(nullptr) {
  for (int i = 0; i < num_threads; ++i) {
    threads_.emplace_back(&DataMover::WorkerLoop, this);
  }
  LOG_INFO("DataMover started with {} threads (no default UFS)", num_threads);
}

DataMover::~DataMover() { Stop(); }

Status DataMover::SubmitPreload(BlockId block_id, const std::string &ufs_path,
                                uint64_t offset, uint64_t length) {
  return SubmitPreload(block_id, ufs_path, offset, length, nullptr);
}

Status DataMover::SubmitPreload(BlockId block_id, const std::string &ufs_path,
                                uint64_t offset, uint64_t length,
                                std::shared_ptr<UnderFileSystem> ufs) {
  Task task;
  task.type = Task::kPreload;
  task.block_id = block_id;
  task.ufs_path = ufs_path;
  task.offset_in_ufs = offset;
  task.length = length;
  task.owned_ufs = std::move(ufs);

  {
    std::lock_guard<std::mutex> lock(mu_);
    tasks_.push(std::move(task));
  }
  cv_.notify_one();
  return Status::OK();
}

Status DataMover::SubmitPersist(BlockId block_id, const std::string &ufs_path,
                                uint64_t offset) {
  return SubmitPersist(block_id, ufs_path, offset, nullptr);
}

Status DataMover::SubmitPersist(BlockId block_id, const std::string &ufs_path,
                                uint64_t offset,
                                std::shared_ptr<UnderFileSystem> ufs) {
  Task task;
  task.type = Task::kPersist;
  task.block_id = block_id;
  task.ufs_path = ufs_path;
  task.offset_in_ufs = offset;
  task.owned_ufs = std::move(ufs);

  {
    std::lock_guard<std::mutex> lock(mu_);
    tasks_.push(std::move(task));
  }
  cv_.notify_one();
  return Status::OK();
}

void DataMover::WaitAll() {
  std::unique_lock<std::mutex> lock(mu_);
  done_cv_.wait(lock, [this] { return tasks_.empty() && active_tasks_ == 0; });
}

void DataMover::Stop() {
  running_ = false;
  cv_.notify_all();
  for (auto &t : threads_) {
    if (t.joinable())
      t.join();
  }
  threads_.clear();
}

size_t DataMover::GetPendingTaskCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return tasks_.size();
}

void DataMover::WorkerLoop() {
  while (running_) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return !tasks_.empty() || !running_; });
      if (!running_ && tasks_.empty())
        return;
      task = tasks_.front();
      tasks_.pop();
      active_tasks_++;
    }

    auto s = ExecuteTask(task);
    if (!s.ok()) {
      LOG_WARN("DataMover task failed: {}", s.ToString());
    }

    active_tasks_--;
    done_cv_.notify_all();
  }
}

Status DataMover::ExecuteTask(const Task &task) {
  // Select UFS: per-task UFS takes precedence over the default
  UnderFileSystem *ufs = task.owned_ufs ? task.owned_ufs.get() : ufs_;
  if (!ufs) {
    return Status::Internal("no UFS available for DataMover task");
  }

  if (task.type == Task::kPreload) {
    // Read from UFS and write into block store
    ScopedLatency lat("data_mover.preload_latency_ms");

    UfsFileHandle handle;
    RETURN_IF_ERROR(ufs->Open(task.ufs_path, O_RDONLY, &handle));

    std::vector<char> buf(task.length);
    size_t bytes_read;
    auto s = ufs->Read(handle, buf.data(), task.length,
                       static_cast<off_t>(task.offset_in_ufs), &bytes_read);
    ufs->Close(handle);
    if (!s.ok())
      return s;

    // Block ID is pre-computed (composite: inode_id + block_index)
    RETURN_IF_ERROR(block_store_->EnsureBlock(task.block_id, bytes_read));
    RETURN_IF_ERROR(
        block_store_->WriteBlock(task.block_id, buf.data(), bytes_read, 0));

    Metrics::Instance().IncrCounter("data_mover.preloads");
    LOG_DEBUG("Preloaded {} bytes from {} into block {}", bytes_read,
              task.ufs_path, task.block_id);
    return Status::OK();

  } else if (task.type == Task::kPersist) {
    // Read from block store and write to UFS
    ScopedLatency lat("data_mover.persist_latency_ms");

    BlockMeta meta;
    RETURN_IF_ERROR(block_store_->GetBlockMeta(task.block_id, &meta));

    std::vector<char> buf(meta.length);
    RETURN_IF_ERROR(
        block_store_->ReadBlock(task.block_id, buf.data(), meta.length, 0));

    UfsFileHandle handle;
    CreateOptions copts;
    copts.recursive = true;
    RETURN_IF_ERROR(ufs->Create(task.ufs_path, copts, &handle));

    size_t bytes_written;
    auto s = ufs->Write(handle, buf.data(), meta.length,
                        static_cast<off_t>(task.offset_in_ufs), &bytes_written);
    ufs->Close(handle);
    if (!s.ok())
      return s;

    Metrics::Instance().IncrCounter("data_mover.persists");
    LOG_DEBUG("Persisted block {} ({} bytes) to {}", task.block_id,
              bytes_written, task.ufs_path);
    return Status::OK();
  }

  return Status::InvalidArgument("unknown task type");
}

} // namespace anycache
