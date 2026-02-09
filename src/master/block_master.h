#pragma once

#include "common/status.h"
#include "common/types.h"

#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace anycache {

// BlockMaster tracks which workers hold which blocks.
class BlockMaster {
public:
  BlockMaster() = default;

  // Get locations for a set of blocks
  Status GetBlockLocations(const std::vector<BlockId> &block_ids,
                           std::vector<BlockLocationInfo> *locations) const;

  // Report that a worker has a block
  void AddBlockLocation(BlockId block_id, WorkerId worker_id,
                        const std::string &address, TierType tier);

  // Remove a block location (e.g., worker evicted it)
  void RemoveBlockLocation(BlockId block_id, WorkerId worker_id);

  // Remove all locations for a worker (worker went down)
  void RemoveWorkerBlocks(WorkerId worker_id);

  // Remove all locations for a block
  void RemoveBlock(BlockId block_id);

  // Get all blocks on a worker
  std::vector<BlockId> GetWorkerBlocks(WorkerId worker_id) const;

  // Get the number of replicas for a block
  size_t GetReplicaCount(BlockId block_id) const;

private:
  mutable std::mutex mu_;
  // block_id -> set of locations
  std::unordered_map<BlockId, std::vector<BlockLocationInfo>> block_locations_;
  // worker_id -> set of block_ids
  std::unordered_map<WorkerId, std::set<BlockId>> worker_blocks_;
};

} // namespace anycache
