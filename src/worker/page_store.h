#pragma once

#include "common/status.h"
#include "common/types.h"
#include "ufs/ufs.h"

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace anycache {

// Thread-safe concurrent LRU cache for pages
template <typename K, typename V> class ConcurrentLRUCache {
public:
  explicit ConcurrentLRUCache(size_t max_entries) : max_entries_(max_entries) {}

  bool Get(const K &key, V *value) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it == map_.end())
      return false;
    // Move to back (most recently used)
    order_.splice(order_.end(), order_, it->second);
    *value = it->second->second;
    return true;
  }

  void Put(const K &key, V value) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      it->second->second = std::move(value);
      order_.splice(order_.end(), order_, it->second);
      return;
    }
    if (map_.size() >= max_entries_) {
      // Evict LRU
      auto &front = order_.front();
      map_.erase(front.first);
      order_.pop_front();
    }
    order_.emplace_back(key, std::move(value));
    map_[key] = std::prev(order_.end());
  }

  bool Contains(const K &key) const {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.count(key) > 0;
  }

  void Erase(const K &key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      order_.erase(it->second);
      map_.erase(it);
    }
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    map_.clear();
    order_.clear();
  }

  // Evict `count` LRU entries, returns keys evicted
  std::vector<K> EvictN(size_t count) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<K> evicted;
    while (evicted.size() < count && !order_.empty()) {
      auto &front = order_.front();
      evicted.push_back(front.first);
      map_.erase(front.first);
      order_.pop_front();
    }
    return evicted;
  }

private:
  size_t max_entries_;
  mutable std::mutex mu_;
  std::list<std::pair<K, V>> order_;
  std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> map_;
};

// Page data container
struct PageData {
  std::vector<char> data;
  bool dirty = false;
};

// PageStore: Page-level cache sitting on top of UFS.
// Caches data in page_size chunks for efficient random reads.
class PageStore {
public:
  // page_fetcher: function that reads a page from the backing store
  using PageFetcher = std::function<Status(FileId file_id, uint64_t page_index,
                                           void *buf, size_t *bytes_read)>;

  PageStore(size_t page_size, size_t max_pages);

  // Read data through the page cache
  Status ReadPage(FileId file_id, uint64_t page_index, void *buf,
                  size_t *bytes_read);

  // Write data into the page cache (marks page dirty)
  Status WritePage(FileId file_id, uint64_t page_index, const void *buf,
                   size_t size);

  // Pre-fetch adjacent pages (async hint)
  void PrefetchPages(FileId file_id, uint64_t start_page, uint32_t count);

  // Evict pages to free memory
  void Evict(size_t pages_to_free);

  // Invalidate all pages for a file
  void InvalidateFile(FileId file_id);

  // Set the fetcher for loading pages from UFS
  void SetPageFetcher(PageFetcher fetcher) { fetcher_ = std::move(fetcher); }

  size_t GetPageSize() const { return page_size_; }
  size_t GetCachedPageCount() const { return cache_.Size(); }

private:
  size_t page_size_;
  ConcurrentLRUCache<PageKey, std::shared_ptr<PageData>> cache_;
  PageFetcher fetcher_;

  // Reverse index: file_id -> set of page_indices cached for that file.
  // Protected by file_index_mu_.
  std::mutex file_index_mu_;
  std::unordered_map<FileId, std::unordered_set<uint64_t>> file_page_index_;

  void TrackPage(FileId file_id, uint64_t page_index);
  void UntrackFile(FileId file_id);
};

} // namespace anycache
