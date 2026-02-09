#include "master/block_master.h"
#include "common/logging.h"

namespace anycache {

Status BlockMaster::GetBlockLocations(
    const std::vector<BlockId> &block_ids,
    std::vector<BlockLocationInfo> *locations) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto bid : block_ids) {
    auto it = block_locations_.find(bid);
    if (it != block_locations_.end()) {
      for (auto &loc : it->second) {
        locations->push_back(loc);
      }
    }
  }
  return Status::OK();
}

void BlockMaster::AddBlockLocation(BlockId block_id, WorkerId worker_id,
                                   const std::string &address, TierType tier) {
  std::lock_guard<std::mutex> lock(mu_);

  // Check if this worker already has this block
  auto &locs = block_locations_[block_id];
  for (auto &loc : locs) {
    if (loc.worker_id == worker_id) {
      loc.tier = tier;
      return;
    }
  }

  BlockLocationInfo info;
  info.block_id = block_id;
  info.worker_id = worker_id;
  info.worker_address = address;
  info.tier = tier;
  locs.push_back(info);

  worker_blocks_[worker_id].insert(block_id);
}

void BlockMaster::RemoveBlockLocation(BlockId block_id, WorkerId worker_id) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = block_locations_.find(block_id);
  if (it != block_locations_.end()) {
    auto &locs = it->second;
    locs.erase(std::remove_if(locs.begin(), locs.end(),
                              [worker_id](const BlockLocationInfo &l) {
                                return l.worker_id == worker_id;
                              }),
               locs.end());
    if (locs.empty()) {
      block_locations_.erase(it);
    }
  }

  auto wit = worker_blocks_.find(worker_id);
  if (wit != worker_blocks_.end()) {
    wit->second.erase(block_id);
  }
}

void BlockMaster::RemoveWorkerBlocks(WorkerId worker_id) {
  std::lock_guard<std::mutex> lock(mu_);

  auto wit = worker_blocks_.find(worker_id);
  if (wit == worker_blocks_.end())
    return;

  for (BlockId bid : wit->second) {
    auto bit = block_locations_.find(bid);
    if (bit != block_locations_.end()) {
      auto &locs = bit->second;
      locs.erase(std::remove_if(locs.begin(), locs.end(),
                                [worker_id](const BlockLocationInfo &l) {
                                  return l.worker_id == worker_id;
                                }),
                 locs.end());
      if (locs.empty())
        block_locations_.erase(bit);
    }
  }
  worker_blocks_.erase(wit);
}

void BlockMaster::RemoveBlock(BlockId block_id) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = block_locations_.find(block_id);
  if (it != block_locations_.end()) {
    for (auto &loc : it->second) {
      auto wit = worker_blocks_.find(loc.worker_id);
      if (wit != worker_blocks_.end()) {
        wit->second.erase(block_id);
      }
    }
    block_locations_.erase(it);
  }
}

std::vector<BlockId> BlockMaster::GetWorkerBlocks(WorkerId worker_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = worker_blocks_.find(worker_id);
  if (it == worker_blocks_.end())
    return {};
  return {it->second.begin(), it->second.end()};
}

size_t BlockMaster::GetReplicaCount(BlockId block_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = block_locations_.find(block_id);
  if (it == block_locations_.end())
    return 0;
  return it->second.size();
}

} // namespace anycache
