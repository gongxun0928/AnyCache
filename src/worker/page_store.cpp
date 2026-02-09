#include "worker/page_store.h"
#include "common/logging.h"
#include "common/metrics.h"

namespace anycache {

PageStore::PageStore(size_t page_size, size_t max_pages)
    : page_size_(page_size), cache_(max_pages) {}

Status PageStore::ReadPage(FileId file_id, uint64_t page_index, void *buf,
                           size_t *bytes_read) {
  PageKey key{file_id, page_index};

  // Try cache hit
  std::shared_ptr<PageData> page;
  if (cache_.Get(key, &page)) {
    Metrics::Instance().IncrCounter("page_store.cache_hits");
    size_t copy_size = page->data.size();
    std::memcpy(buf, page->data.data(), copy_size);
    if (bytes_read)
      *bytes_read = copy_size;
    return Status::OK();
  }

  // Cache miss: fetch from backing store
  Metrics::Instance().IncrCounter("page_store.cache_misses");

  if (!fetcher_) {
    return Status::Internal("no page fetcher configured");
  }

  page = std::make_shared<PageData>();
  page->data.resize(page_size_);
  size_t fetched = 0;
  RETURN_IF_ERROR(fetcher_(file_id, page_index, page->data.data(), &fetched));
  page->data.resize(fetched);

  cache_.Put(key, page);
  TrackPage(file_id, page_index);

  std::memcpy(buf, page->data.data(), fetched);
  if (bytes_read)
    *bytes_read = fetched;
  return Status::OK();
}

Status PageStore::WritePage(FileId file_id, uint64_t page_index,
                            const void *buf, size_t size) {
  PageKey key{file_id, page_index};

  auto page = std::make_shared<PageData>();
  page->data.assign(static_cast<const char *>(buf),
                    static_cast<const char *>(buf) + size);
  page->dirty = true;

  cache_.Put(key, page);
  TrackPage(file_id, page_index);
  Metrics::Instance().IncrCounter("page_store.writes");
  return Status::OK();
}

void PageStore::PrefetchPages(FileId file_id, uint64_t start_page,
                              uint32_t count) {
  // Simple sync prefetch; a real implementation would use async I/O
  for (uint32_t i = 0; i < count; ++i) {
    PageKey key{file_id, start_page + i};
    if (cache_.Contains(key))
      continue;

    if (fetcher_) {
      auto page = std::make_shared<PageData>();
      page->data.resize(page_size_);
      size_t fetched = 0;
      auto s = fetcher_(file_id, start_page + i, page->data.data(), &fetched);
      if (s.ok()) {
        page->data.resize(fetched);
        cache_.Put(key, page);
        TrackPage(file_id, start_page + i);
      }
    }
  }
  Metrics::Instance().IncrCounter("page_store.prefetches", count);
}

void PageStore::Evict(size_t pages_to_free) {
  cache_.EvictN(pages_to_free);
  Metrics::Instance().IncrCounter("page_store.evictions", pages_to_free);
}

void PageStore::InvalidateFile(FileId file_id) {
  std::unordered_set<uint64_t> pages;
  {
    std::lock_guard<std::mutex> lock(file_index_mu_);
    auto it = file_page_index_.find(file_id);
    if (it == file_page_index_.end())
      return;
    pages = std::move(it->second);
    file_page_index_.erase(it);
  }

  for (uint64_t page_idx : pages) {
    cache_.Erase(PageKey{file_id, page_idx});
  }

  Metrics::Instance().IncrCounter("page_store.file_invalidations");
  LOG_DEBUG("Invalidated {} pages for file {}", pages.size(), file_id);
}

void PageStore::TrackPage(FileId file_id, uint64_t page_index) {
  std::lock_guard<std::mutex> lock(file_index_mu_);
  file_page_index_[file_id].insert(page_index);
}

void PageStore::UntrackFile(FileId file_id) {
  std::lock_guard<std::mutex> lock(file_index_mu_);
  file_page_index_.erase(file_id);
}

} // namespace anycache
