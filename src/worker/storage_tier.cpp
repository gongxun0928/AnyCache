#include "worker/storage_tier.h"
#include "common/logging.h"

#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace anycache {

StorageTier::StorageTier(TierType type, const std::string &path,
                         size_t capacity)
    : type_(type), path_(path), capacity_(capacity) {
  if (type_ != TierType::kMemory) {
    fs::create_directories(path_);
  }
  LOG_INFO("StorageTier created: type={}, path={}, capacity={}MB",
           TierTypeName(type_), path_, capacity_ / (1024 * 1024));
}

StorageTier::~StorageTier() {
  std::lock_guard<std::mutex> lock(mu_);
  if (type_ == TierType::kMemory) {
    for (auto &[id, handle] : blocks_) {
      if (handle.mem_ptr) {
        free(handle.mem_ptr);
        handle.mem_ptr = nullptr;
      }
    }
  }
}

Status StorageTier::AllocateBlock(BlockId id, size_t size,
                                  BlockHandle *handle) {
  std::lock_guard<std::mutex> lock(mu_);
  if (blocks_.count(id)) {
    return Status::AlreadyExists("block already allocated in tier");
  }
  if (used_bytes_ + size > capacity_) {
    return Status::ResourceExhausted("tier capacity exceeded");
  }

  if (type_ == TierType::kMemory) {
    return AllocateMem(id, size, handle);
  } else {
    return AllocateDisk(id, size, handle);
  }
}

Status StorageTier::ReadBlock(BlockId id, void *buf, size_t size,
                              off_t offset) {
  std::lock_guard<std::mutex> lock(mu_);
  if (type_ == TierType::kMemory)
    return ReadMem(id, buf, size, offset);
  return ReadDisk(id, buf, size, offset);
}

Status StorageTier::WriteBlock(BlockId id, const void *buf, size_t size,
                               off_t offset) {
  std::lock_guard<std::mutex> lock(mu_);
  if (type_ == TierType::kMemory)
    return WriteMem(id, buf, size, offset);
  return WriteDisk(id, buf, size, offset);
}

Status StorageTier::RemoveBlock(BlockId id) {
  std::lock_guard<std::mutex> lock(mu_);
  if (type_ == TierType::kMemory)
    return RemoveMem(id);
  return RemoveDisk(id);
}

bool StorageTier::HasBlock(BlockId id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return blocks_.count(id) > 0;
}

Status StorageTier::ExportBlock(BlockId id, std::vector<char> *data) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = blocks_.find(id);
  if (it == blocks_.end())
    return Status::NotFound("block not in tier");

  auto &handle = it->second;
  data->resize(handle.capacity);
  if (type_ == TierType::kMemory) {
    std::memcpy(data->data(), handle.mem_ptr, handle.capacity);
  } else {
    int fd = ::open(handle.path.c_str(), O_RDONLY);
    if (fd < 0)
      return Status::IOError("open failed");
    ssize_t n = ::pread(fd, data->data(), handle.capacity, 0);
    ::close(fd);
    if (n < 0)
      return Status::IOError("pread failed");
    data->resize(static_cast<size_t>(n));
  }
  return Status::OK();
}

Status StorageTier::ImportBlock(BlockId id, const std::vector<char> &data) {
  BlockHandle handle;
  RETURN_IF_ERROR(AllocateBlock(id, data.size(), &handle));
  return WriteBlock(id, data.data(), data.size(), 0);
}

std::vector<BlockId> StorageTier::GetBlockIds() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<BlockId> ids;
  ids.reserve(blocks_.size());
  for (auto &[id, _] : blocks_) {
    ids.push_back(id);
  }
  return ids;
}

// ─── Memory tier impl ────────────────────────────────────────
Status StorageTier::AllocateMem(BlockId id, size_t size, BlockHandle *handle) {
  void *ptr = malloc(size);
  if (!ptr)
    return Status::ResourceExhausted("malloc failed");
  std::memset(ptr, 0, size);

  BlockHandle bh;
  bh.block_id = id;
  bh.tier = TierType::kMemory;
  bh.mem_ptr = ptr;
  bh.capacity = size;

  blocks_[id] = bh;
  used_bytes_ += size;
  *handle = bh;
  return Status::OK();
}

Status StorageTier::ReadMem(BlockId id, void *buf, size_t size, off_t offset) {
  auto it = blocks_.find(id);
  if (it == blocks_.end())
    return Status::NotFound("block not found");
  auto &h = it->second;
  if (static_cast<size_t>(offset) + size > h.capacity) {
    size = h.capacity - offset;
  }
  std::memcpy(buf, static_cast<char *>(h.mem_ptr) + offset, size);
  return Status::OK();
}

Status StorageTier::WriteMem(BlockId id, const void *buf, size_t size,
                             off_t offset) {
  auto it = blocks_.find(id);
  if (it == blocks_.end())
    return Status::NotFound("block not found");
  auto &h = it->second;
  if (static_cast<size_t>(offset) + size > h.capacity) {
    return Status::InvalidArgument("write exceeds block capacity");
  }
  std::memcpy(static_cast<char *>(h.mem_ptr) + offset, buf, size);
  return Status::OK();
}

Status StorageTier::RemoveMem(BlockId id) {
  auto it = blocks_.find(id);
  if (it == blocks_.end())
    return Status::NotFound("block not found");
  used_bytes_ -= it->second.capacity;
  free(it->second.mem_ptr);
  blocks_.erase(it);
  return Status::OK();
}

// ─── Disk tier impl ──────────────────────────────────────────
std::string StorageTier::BlockFilePath(BlockId id) const {
  return path_ + "/block_" + std::to_string(id);
}

Status StorageTier::AllocateDisk(BlockId id, size_t size, BlockHandle *handle) {
  std::string fpath = BlockFilePath(id);
  int fd = ::open(fpath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0)
    return Status::IOError("create block file failed");

  // Pre-allocate space
  if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
    ::close(fd);
    ::unlink(fpath.c_str());
    return Status::IOError("ftruncate failed");
  }
  ::close(fd);

  BlockHandle bh;
  bh.block_id = id;
  bh.tier = type_;
  bh.path = fpath;
  bh.capacity = size;

  blocks_[id] = bh;
  used_bytes_ += size;
  *handle = bh;
  return Status::OK();
}

Status StorageTier::ReadDisk(BlockId id, void *buf, size_t size, off_t offset) {
  auto it = blocks_.find(id);
  if (it == blocks_.end())
    return Status::NotFound("block not found");

  int fd = ::open(it->second.path.c_str(), O_RDONLY);
  if (fd < 0)
    return Status::IOError("open block file failed");
  ssize_t n = ::pread(fd, buf, size, offset);
  ::close(fd);
  if (n < 0)
    return Status::IOError("pread failed");
  return Status::OK();
}

Status StorageTier::WriteDisk(BlockId id, const void *buf, size_t size,
                              off_t offset) {
  auto it = blocks_.find(id);
  if (it == blocks_.end())
    return Status::NotFound("block not found");

  int fd = ::open(it->second.path.c_str(), O_WRONLY);
  if (fd < 0)
    return Status::IOError("open block file failed");
  ssize_t n = ::pwrite(fd, buf, size, offset);
  ::close(fd);
  if (n < 0)
    return Status::IOError("pwrite failed");
  return Status::OK();
}

Status StorageTier::RemoveDisk(BlockId id) {
  auto it = blocks_.find(id);
  if (it == blocks_.end())
    return Status::NotFound("block not found");
  used_bytes_ -= it->second.capacity;
  ::unlink(it->second.path.c_str());
  blocks_.erase(it);
  return Status::OK();
}

} // namespace anycache
