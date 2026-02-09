#include "worker/block_store.h"
#include "common/logging.h"
#include "common/metrics.h"

#include <chrono>

namespace anycache {

static int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

BlockStore::BlockStore(const Options &opts) : opts_(opts) {
  // Create storage tiers
  for (auto &tc : opts.tiers) {
    tiers_.push_back(
        std::make_unique<StorageTier>(tc.type, tc.path, tc.capacity_bytes));
  }
  // Sort tiers: Memory first, then SSD, then HDD
  std::sort(tiers_.begin(), tiers_.end(), [](const auto &a, const auto &b) {
    return static_cast<int>(a->GetType()) < static_cast<int>(b->GetType());
  });

  cache_mgr_ = std::make_unique<CacheManager>(opts.cache_policy);
  meta_store_ = MetaStore::Create(opts.meta_db_path);
}

Status BlockStore::CreateBlock(BlockId block_id, size_t size) {
  // Try to allocate in the fastest tier with space
  StorageTier *target = nullptr;
  for (auto &tier : tiers_) {
    if (tier->GetAvailableBytes() >= size) {
      target = tier.get();
      break;
    }
  }

  if (!target) {
    // Try eviction from the fastest tier
    std::vector<BlockId> evicted;
    RETURN_IF_ERROR(EvictBlocks(tiers_.front()->GetType(), size, &evicted));
    target = tiers_.front().get();
    if (target->GetAvailableBytes() < size) {
      return Status::ResourceExhausted("no tier has enough space");
    }
  }

  BlockHandle handle;
  RETURN_IF_ERROR(target->AllocateBlock(block_id, size, &handle));

  // Update metadata
  BlockMeta meta;
  meta.block_id = block_id;
  meta.length = size;
  meta.tier = target->GetType();
  meta.create_time_ms = NowMs();
  meta.last_access_time_ms = meta.create_time_ms;
  RETURN_IF_ERROR(meta_store_->PutBlockMeta(block_id, meta));

  {
    std::lock_guard<std::mutex> lock(mu_);
    block_tier_map_[block_id] = target->GetType();
  }

  cache_mgr_->OnBlockInsert(block_id, size);
  Metrics::Instance().IncrCounter("block_store.blocks_created");

  // Check if any tier needs proactive eviction
  MaybeAutoEvict(target->GetType());

  return Status::OK();
}

Status BlockStore::EnsureBlock(BlockId block_id, size_t size) {
  if (HasBlock(block_id)) {
    return Status::OK();
  }
  return CreateBlock(block_id, size);
}

Status BlockStore::ReadBlock(BlockId id, void *buf, size_t size, off_t offset) {
  ScopedLatency lat("block_store.read_latency_ms");

  StorageTier *tier = FindBlockTier(id);
  if (!tier)
    return Status::NotFound("block not cached");

  RETURN_IF_ERROR(tier->ReadBlock(id, buf, size, offset));
  cache_mgr_->OnBlockAccess(id);

  // Update access time in meta
  BlockMeta meta;
  if (meta_store_->GetBlockMeta(id, &meta).ok()) {
    meta.last_access_time_ms = NowMs();
    meta.access_count++;
    meta_store_->PutBlockMeta(id, meta);

    // Auto-promote hot blocks to faster tier
    MaybeAutoPromote(id, meta);
  }

  Metrics::Instance().IncrCounter("block_store.reads");
  return Status::OK();
}

Status BlockStore::WriteBlock(BlockId id, const void *buf, size_t size,
                              off_t offset) {
  ScopedLatency lat("block_store.write_latency_ms");

  StorageTier *tier = FindBlockTier(id);
  if (!tier)
    return Status::NotFound("block not cached");

  RETURN_IF_ERROR(tier->WriteBlock(id, buf, size, offset));
  cache_mgr_->OnBlockAccess(id);

  Metrics::Instance().IncrCounter("block_store.writes");
  return Status::OK();
}

Status BlockStore::RemoveBlock(BlockId id) {
  StorageTier *tier = FindBlockTier(id);
  if (tier) {
    tier->RemoveBlock(id);
  }

  cache_mgr_->OnBlockRemove(id);
  meta_store_->DeleteBlockMeta(id);

  {
    std::lock_guard<std::mutex> lock(mu_);
    block_tier_map_.erase(id);
  }

  Metrics::Instance().IncrCounter("block_store.blocks_removed");
  return Status::OK();
}

Status BlockStore::PromoteBlock(BlockId id, TierType target_type) {
  StorageTier *src_tier = FindBlockTier(id);
  if (!src_tier)
    return Status::NotFound("block not found");
  if (src_tier->GetType() == target_type)
    return Status::OK();

  StorageTier *dst_tier = FindTier(target_type);
  if (!dst_tier)
    return Status::NotFound("target tier not found");

  // Export data from source
  std::vector<char> data;
  RETURN_IF_ERROR(src_tier->ExportBlock(id, &data));

  // Import into destination
  RETURN_IF_ERROR(dst_tier->ImportBlock(id, data));

  // Remove from source
  src_tier->RemoveBlock(id);

  // Update metadata
  BlockMeta meta;
  if (meta_store_->GetBlockMeta(id, &meta).ok()) {
    meta.tier = target_type;
    meta_store_->PutBlockMeta(id, meta);
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    block_tier_map_[id] = target_type;
  }

  Metrics::Instance().IncrCounter("block_store.promotions");
  LOG_DEBUG("Promoted block {} to {}", id, TierTypeName(target_type));
  return Status::OK();
}

Status BlockStore::EvictBlocks(TierType tier, size_t bytes_needed,
                               std::vector<BlockId> *evicted) {
  auto candidates = cache_mgr_->GetEvictionCandidates(bytes_needed);
  for (BlockId bid : candidates) {
    StorageTier *t = FindBlockTier(bid);
    if (t && t->GetType() == tier) {
      t->RemoveBlock(bid);
      meta_store_->DeleteBlockMeta(bid);
      {
        std::lock_guard<std::mutex> lock(mu_);
        block_tier_map_.erase(bid);
      }
      if (evicted)
        evicted->push_back(bid);
    }
  }
  Metrics::Instance().IncrCounter("block_store.evictions",
                                  evicted ? evicted->size() : 0);
  return Status::OK();
}

Status BlockStore::Recover() {
  std::vector<BlockMeta> all_meta;
  RETURN_IF_ERROR(meta_store_->ScanAll(&all_meta));

  size_t recovered = 0;
  for (auto &meta : all_meta) {
    StorageTier *tier = FindTier(meta.tier);
    if (tier && tier->HasBlock(meta.block_id)) {
      std::lock_guard<std::mutex> lock(mu_);
      block_tier_map_[meta.block_id] = meta.tier;
      cache_mgr_->OnBlockInsert(meta.block_id, meta.length);
      recovered++;
    } else {
      // Block data is gone (e.g., memory tier after restart), clean up meta
      meta_store_->DeleteBlockMeta(meta.block_id);
    }
  }
  LOG_INFO("BlockStore recovery: {} blocks recovered from MetaStore",
           recovered);
  return Status::OK();
}

bool BlockStore::HasBlock(BlockId id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return block_tier_map_.count(id) > 0;
}

Status BlockStore::GetBlockMeta(BlockId id, BlockMeta *meta) const {
  return meta_store_->GetBlockMeta(id, meta);
}

size_t BlockStore::GetTierUsedBytes(TierType tier) const {
  auto *t = FindTier(tier);
  return t ? t->GetUsedBytes() : 0;
}

size_t BlockStore::GetTierCapacity(TierType tier) const {
  auto *t = FindTier(tier);
  return t ? t->GetCapacity() : 0;
}

size_t BlockStore::GetTotalCachedBytes() const {
  return cache_mgr_->GetCachedBytes();
}

StorageTier *BlockStore::FindTier(TierType type) {
  for (auto &t : tiers_) {
    if (t->GetType() == type)
      return t.get();
  }
  return nullptr;
}

const StorageTier *BlockStore::FindTier(TierType type) const {
  for (auto &t : tiers_) {
    if (t->GetType() == type)
      return t.get();
  }
  return nullptr;
}

void BlockStore::MaybeAutoPromote(BlockId id, const BlockMeta &meta) {
  if (opts_.auto_promote_access_threshold == 0)
    return;
  if (meta.access_count < opts_.auto_promote_access_threshold)
    return;

  // Find current tier
  TierType current_tier;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = block_tier_map_.find(id);
    if (it == block_tier_map_.end())
      return;
    current_tier = it->second;
  }

  // Find next faster tier (lower enum value = faster)
  TierType target = current_tier;
  if (current_tier == TierType::kHDD)
    target = TierType::kSSD;
  else if (current_tier == TierType::kSSD)
    target = TierType::kMemory;
  else
    return; // Already at fastest tier

  // Only promote if target tier exists and has space
  StorageTier *dst = FindTier(target);
  if (!dst || dst->GetAvailableBytes() < meta.length)
    return;

  auto s = PromoteBlock(id, target);
  if (s.ok()) {
    LOG_DEBUG("Auto-promoted block {} from {} to {} (access_count={})", id,
              TierTypeName(current_tier), TierTypeName(target),
              meta.access_count);
  }
}

void BlockStore::MaybeAutoEvict(TierType tier) {
  StorageTier *t = FindTier(tier);
  if (!t || t->GetCapacity() == 0)
    return;

  double usage = static_cast<double>(t->GetUsedBytes()) / t->GetCapacity();
  if (usage <= opts_.auto_evict_high_watermark)
    return;

  // Evict down to low watermark
  size_t target_used =
      static_cast<size_t>(t->GetCapacity() * opts_.auto_evict_low_watermark);
  size_t to_free =
      t->GetUsedBytes() > target_used ? t->GetUsedBytes() - target_used : 0;
  if (to_free == 0)
    return;

  std::vector<BlockId> evicted;
  EvictBlocks(tier, to_free, &evicted);
  if (!evicted.empty()) {
    LOG_DEBUG("Auto-evicted {} blocks from {} (freed ~{} bytes, usage {:.1f}%)",
              evicted.size(), TierTypeName(tier), to_free, usage * 100);
  }
}

StorageTier *BlockStore::FindBlockTier(BlockId id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = block_tier_map_.find(id);
  if (it == block_tier_map_.end())
    return nullptr;
  return FindTier(it->second);
}

} // namespace anycache
