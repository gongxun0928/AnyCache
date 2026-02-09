#pragma once

#include "common/config.h"
#include "common/status.h"
#include "common/types.h"
#include "worker/cache_manager.h"
#include "worker/meta_store.h"
#include "worker/storage_tier.h"

#include <memory>
#include <mutex>
#include <vector>

namespace anycache {

// BlockStore manages all storage tiers and coordinates reads/writes
// with the CacheManager for eviction and the MetaStore for persistence.
class BlockStore {
public:
  struct Options {
    std::vector<TierConfig> tiers;
    std::string meta_db_path = "/tmp/anycache/meta";
    CacheManager::PolicyType cache_policy = CacheManager::PolicyType::kLRU;

    // Auto-promotion: promote a block to faster tier after this many accesses.
    // 0 = disabled.
    uint32_t auto_promote_access_threshold = 3;

    // Auto-eviction: when a tier is above this usage ratio (0.0–1.0), trigger
    // eviction to free space proactively.
    double auto_evict_high_watermark = 0.95;
    // Evict down to this ratio.
    double auto_evict_low_watermark = 0.80;
  };

  explicit BlockStore(const Options &opts);
  ~BlockStore() = default;

  // Create a block with a deterministic BlockId (composite: inode_id + index)
  Status CreateBlock(BlockId block_id, size_t size);

  // Ensure a block exists; create it if absent, no-op if already present
  Status EnsureBlock(BlockId block_id, size_t size);

  // Read from a cached block
  Status ReadBlock(BlockId id, void *buf, size_t size, off_t offset);

  // Write to a block
  Status WriteBlock(BlockId id, const void *buf, size_t size, off_t offset);

  // Remove a block from all tiers
  Status RemoveBlock(BlockId id);

  // Promote a block to a faster tier
  Status PromoteBlock(BlockId id, TierType target_tier);

  // Evict blocks to free space in a tier; returns evicted block ids
  Status EvictBlocks(TierType tier, size_t bytes_needed,
                     std::vector<BlockId> *evicted);

  // Recovery: reload block index from MetaStore (RocksDB)
  Status Recover();

  // Query
  bool HasBlock(BlockId id) const;
  Status GetBlockMeta(BlockId id, BlockMeta *meta) const;

  // Metrics
  size_t GetTierUsedBytes(TierType tier) const;
  size_t GetTierCapacity(TierType tier) const;
  size_t GetTotalCachedBytes() const;

private:
  StorageTier *FindTier(TierType type);
  const StorageTier *FindTier(TierType type) const;
  StorageTier *FindBlockTier(BlockId id);

  // Try to auto-promote a block to a faster tier based on access count.
  void MaybeAutoPromote(BlockId id, const BlockMeta &meta);

  // Check tier usage and trigger eviction if above high watermark.
  void MaybeAutoEvict(TierType tier);

  Options opts_;
  std::vector<std::unique_ptr<StorageTier>> tiers_;
  std::unique_ptr<CacheManager> cache_mgr_;
  std::unique_ptr<MetaStore> meta_store_;

  mutable std::mutex mu_;
  // block_id -> which tier it's in
  std::unordered_map<BlockId, TierType> block_tier_map_;
};

} // namespace anycache
