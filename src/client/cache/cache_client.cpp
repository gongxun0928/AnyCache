#include "client/cache/cache_client.h"

namespace anycache {

CacheClient::CacheClient(size_t page_size, size_t max_pages)
    : page_store_(std::make_unique<PageStore>(page_size, max_pages)) {}

Status CacheClient::ReadCached(FileId file_id, uint64_t offset, void *buf,
                               size_t size, size_t *bytes_read) {
  size_t page_size = page_store_->GetPageSize();
  size_t total_read = 0;
  auto *dst = static_cast<char *>(buf);

  while (total_read < size) {
    uint64_t abs_offset = offset + total_read;
    uint64_t page_index = abs_offset / page_size;
    size_t page_offset = abs_offset % page_size;

    std::vector<char> page_buf(page_size);
    size_t page_read = 0;
    auto s =
        page_store_->ReadPage(file_id, page_index, page_buf.data(), &page_read);
    if (!s.ok()) {
      // If we've already read some data, return partial result
      if (total_read > 0)
        break;
      return s;
    }

    // No data available from this page offset
    if (page_read <= page_offset)
      break;

    size_t available = page_read - page_offset;
    size_t to_copy = std::min(size - total_read, available);
    std::memcpy(dst + total_read, page_buf.data() + page_offset, to_copy);
    total_read += to_copy;

    // If we got less than a full page, we've reached EOF
    if (page_read < page_size)
      break;
  }

  *bytes_read = total_read;
  return Status::OK();
}

void CacheClient::InvalidateFile(FileId file_id) {
  page_store_->InvalidateFile(file_id);
}

} // namespace anycache
