#pragma once

#include "common/status.h"
#include "common/types.h"
#include "worker/page_store.h"

#include <memory>

namespace anycache {

// CacheClient provides client-side caching (optional local page cache).
// Optional component: links anycache_client + anycache_worker.
class CacheClient {
public:
  CacheClient(size_t page_size = kDefaultPageSize, size_t max_pages = 256);

  Status ReadCached(FileId file_id, uint64_t offset, void *buf, size_t size,
                    size_t *bytes_read);

  void InvalidateFile(FileId file_id);

  PageStore &GetPageStore() { return *page_store_; }

private:
  std::unique_ptr<PageStore> page_store_;
};

} // namespace anycache
