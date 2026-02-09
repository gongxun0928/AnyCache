#include "worker/cache_manager.h"
#include "common/logging.h"

namespace anycache {

// ═══════════════════════════════════════════════════════════════
// LRU Policy
// ═══════════════════════════════════════════════════════════════
void LRUPolicy::OnAccess(BlockId id) {
  auto it = map_.find(id);
  if (it != map_.end()) {
    order_.erase(it->second);
    order_.push_back(id);
    it->second = std::prev(order_.end());
  }
}

void LRUPolicy::OnInsert(BlockId id) {
  // Remove if exists, then add to back (most recently used)
  auto it = map_.find(id);
  if (it != map_.end()) {
    order_.erase(it->second);
  }
  order_.push_back(id);
  map_[id] = std::prev(order_.end());
}

void LRUPolicy::OnRemove(BlockId id) {
  auto it = map_.find(id);
  if (it != map_.end()) {
    order_.erase(it->second);
    map_.erase(it);
  }
}

BlockId LRUPolicy::Evict() {
  if (order_.empty())
    return kInvalidBlockId;
  BlockId victim = order_.front();
  order_.pop_front();
  map_.erase(victim);
  return victim;
}

size_t LRUPolicy::Size() const { return map_.size(); }

// ═══════════════════════════════════════════════════════════════
// LFU Policy
// ═══════════════════════════════════════════════════════════════
void LFUPolicy::OnAccess(BlockId id) {
  auto fit = freq_map_.find(id);
  if (fit == freq_map_.end())
    return;

  uint64_t old_freq = fit->second;
  uint64_t new_freq = old_freq + 1;
  fit->second = new_freq;

  // Remove from old freq list
  auto &old_list = freq_lists_[old_freq];
  old_list.remove(id);
  if (old_list.empty()) {
    freq_lists_.erase(old_freq);
    if (min_freq_ == old_freq)
      min_freq_ = new_freq;
  }

  // Add to new freq list
  freq_lists_[new_freq].push_back(id);
}

void LFUPolicy::OnInsert(BlockId id) {
  // New entry starts at freq 1
  auto fit = freq_map_.find(id);
  if (fit != freq_map_.end()) {
    OnAccess(id);
    return;
  }

  freq_map_[id] = 1;
  freq_lists_[1].push_back(id);
  min_freq_ = 1;
}

void LFUPolicy::OnRemove(BlockId id) {
  auto fit = freq_map_.find(id);
  if (fit == freq_map_.end())
    return;

  uint64_t freq = fit->second;
  auto &lst = freq_lists_[freq];
  lst.remove(id);
  if (lst.empty()) {
    freq_lists_.erase(freq);
  }
  freq_map_.erase(fit);
}

BlockId LFUPolicy::Evict() {
  if (freq_map_.empty())
    return kInvalidBlockId;

  // Find the min_freq list
  while (freq_lists_.find(min_freq_) == freq_lists_.end() ||
         freq_lists_[min_freq_].empty()) {
    if (freq_map_.empty())
      return kInvalidBlockId;
    min_freq_++;
  }

  auto &lst = freq_lists_[min_freq_];
  BlockId victim = lst.front();
  lst.pop_front();
  if (lst.empty())
    freq_lists_.erase(min_freq_);
  freq_map_.erase(victim);
  return victim;
}

size_t LFUPolicy::Size() const { return freq_map_.size(); }

// ═══════════════════════════════════════════════════════════════
// CacheManager
// ═══════════════════════════════════════════════════════════════
CacheManager::CacheManager(PolicyType type) {
  switch (type) {
  case PolicyType::kLRU:
    policy_ = std::make_unique<LRUPolicy>();
    break;
  case PolicyType::kLFU:
    policy_ = std::make_unique<LFUPolicy>();
    break;
  }
}

void CacheManager::OnBlockAccess(BlockId id) {
  std::lock_guard<std::mutex> lock(mu_);
  policy_->OnAccess(id);
}

void CacheManager::OnBlockInsert(BlockId id, size_t size) {
  std::lock_guard<std::mutex> lock(mu_);
  policy_->OnInsert(id);
  block_sizes_[id] = size;
  total_cached_bytes_ += size;
}

void CacheManager::OnBlockRemove(BlockId id) {
  std::lock_guard<std::mutex> lock(mu_);
  policy_->OnRemove(id);
  auto it = block_sizes_.find(id);
  if (it != block_sizes_.end()) {
    total_cached_bytes_ -= it->second;
    block_sizes_.erase(it);
  }
}

std::vector<BlockId> CacheManager::GetEvictionCandidates(size_t bytes_needed) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<BlockId> victims;
  size_t freed = 0;
  while (freed < bytes_needed && policy_->Size() > 0) {
    BlockId victim = policy_->Evict();
    if (victim == kInvalidBlockId)
      break;
    auto it = block_sizes_.find(victim);
    if (it != block_sizes_.end()) {
      freed += it->second;
      total_cached_bytes_ -= it->second;
      block_sizes_.erase(it);
    }
    victims.push_back(victim);
  }
  return victims;
}

size_t CacheManager::GetCachedBlockCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return block_sizes_.size();
}

size_t CacheManager::GetCachedBytes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return total_cached_bytes_;
}

} // namespace anycache
