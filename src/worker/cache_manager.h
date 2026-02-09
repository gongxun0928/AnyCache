#pragma once

#include "common/status.h"
#include "common/types.h"

#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace anycache {

// ─── Cache eviction policy interface ─────────────────────────
class CachePolicy {
public:
  virtual ~CachePolicy() = default;
  virtual void OnAccess(BlockId id) = 0;
  virtual void OnInsert(BlockId id) = 0;
  virtual void OnRemove(BlockId id) = 0;
  virtual BlockId
  Evict() = 0; // Returns block to evict, kInvalidBlockId if empty
  virtual size_t Size() const = 0;
};

// ─── LRU policy ──────────────────────────────────────────────
class LRUPolicy : public CachePolicy {
public:
  void OnAccess(BlockId id) override;
  void OnInsert(BlockId id) override;
  void OnRemove(BlockId id) override;
  BlockId Evict() override;
  size_t Size() const override;

private:
  std::list<BlockId> order_; // front = LRU (victim)
  std::unordered_map<BlockId, std::list<BlockId>::iterator> map_;
};

// ─── LFU policy ──────────────────────────────────────────────
class LFUPolicy : public CachePolicy {
public:
  void OnAccess(BlockId id) override;
  void OnInsert(BlockId id) override;
  void OnRemove(BlockId id) override;
  BlockId Evict() override;
  size_t Size() const override;

private:
  struct Entry {
    BlockId id;
    uint64_t freq;
  };
  std::unordered_map<BlockId, uint64_t> freq_map_;
  // freq -> list of block ids (front = oldest at this freq)
  std::unordered_map<uint64_t, std::list<BlockId>> freq_lists_;
  uint64_t min_freq_ = 0;
};

// ─── CacheManager ────────────────────────────────────────────
// Coordinates eviction across tiers using a pluggable CachePolicy.
class CacheManager {
public:
  enum class PolicyType { kLRU, kLFU };

  explicit CacheManager(PolicyType type = PolicyType::kLRU);

  // Notify the manager of block access / insertion / removal
  void OnBlockAccess(BlockId id);
  void OnBlockInsert(BlockId id, size_t size);
  void OnBlockRemove(BlockId id);

  // Get blocks to evict to free at least `bytes_needed`
  std::vector<BlockId> GetEvictionCandidates(size_t bytes_needed);

  size_t GetCachedBlockCount() const;
  size_t GetCachedBytes() const;

private:
  mutable std::mutex mu_;
  std::unique_ptr<CachePolicy> policy_;
  std::unordered_map<BlockId, size_t> block_sizes_;
  size_t total_cached_bytes_ = 0;
};

} // namespace anycache
