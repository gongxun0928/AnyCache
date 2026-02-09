#pragma once

#include "common/status.h"
#include "common/types.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace anycache {

// Handle to an allocated block within a tier
struct BlockHandle {
  BlockId block_id = kInvalidBlockId;
  TierType tier = TierType::kMemory;
  std::string path;        // file path on disk (SSD/HDD) or empty for memory
  void *mem_ptr = nullptr; // memory pointer for memory tier
  size_t capacity = 0;
};

// A single storage tier (Memory / SSD / HDD)
class StorageTier {
public:
  StorageTier(TierType type, const std::string &path, size_t capacity);
  ~StorageTier();

  // Allocate space for a block
  Status AllocateBlock(BlockId id, size_t size, BlockHandle *handle);

  // Read data from a block
  Status ReadBlock(BlockId id, void *buf, size_t size, off_t offset);

  // Write data to a block
  Status WriteBlock(BlockId id, const void *buf, size_t size, off_t offset);

  // Remove a block, freeing its space
  Status RemoveBlock(BlockId id);

  // Check if a block exists in this tier
  bool HasBlock(BlockId id) const;

  // Move a block's data out (returns ownership of data)
  Status ExportBlock(BlockId id, std::vector<char> *data);

  // Import block data into this tier
  Status ImportBlock(BlockId id, const std::vector<char> &data);

  // Getters
  TierType GetType() const { return type_; }
  size_t GetUsedBytes() const { return used_bytes_; }
  size_t GetCapacity() const { return capacity_; }
  size_t GetAvailableBytes() const { return capacity_ - used_bytes_; }
  const std::string &GetPath() const { return path_; }

  // Get all block IDs in this tier
  std::vector<BlockId> GetBlockIds() const;

private:
  // Memory tier: allocates from heap
  Status AllocateMem(BlockId id, size_t size, BlockHandle *handle);
  Status ReadMem(BlockId id, void *buf, size_t size, off_t offset);
  Status WriteMem(BlockId id, const void *buf, size_t size, off_t offset);
  Status RemoveMem(BlockId id);

  // Disk tier (SSD/HDD): uses files under path_
  Status AllocateDisk(BlockId id, size_t size, BlockHandle *handle);
  Status ReadDisk(BlockId id, void *buf, size_t size, off_t offset);
  Status WriteDisk(BlockId id, const void *buf, size_t size, off_t offset);
  Status RemoveDisk(BlockId id);

  std::string BlockFilePath(BlockId id) const;

  TierType type_;
  std::string path_;
  size_t capacity_;
  size_t used_bytes_ = 0;

  mutable std::mutex mu_;
  std::unordered_map<BlockId, BlockHandle> blocks_;
};

} // namespace anycache
