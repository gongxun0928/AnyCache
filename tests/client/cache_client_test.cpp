#include "client/cache/cache_client.h"
#include <gtest/gtest.h>

#include <cstring>

using namespace anycache;

class CacheClientTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Use small page size for easier testing
    client_ = std::make_unique<CacheClient>(/*page_size=*/16, /*max_pages=*/32);

    // Set up a page fetcher that returns sequential bytes
    client_->GetPageStore().SetPageFetcher(
        [this](FileId file_id, uint64_t page_index, void *buf,
               size_t *bytes_read) -> Status {
          // Generate deterministic data: page content is page_index repeated
          size_t page_size = 16;
          auto *dst = static_cast<char *>(buf);
          for (size_t i = 0; i < page_size; ++i) {
            dst[i] = static_cast<char>('A' + (page_index % 26));
          }
          *bytes_read = page_size;
          return Status::OK();
        });
  }

  std::unique_ptr<CacheClient> client_;
};

TEST_F(CacheClientTest, ReadWithinSinglePage) {
  char buf[8];
  size_t read = 0;
  auto s = client_->ReadCached(1, 0, buf, 8, &read);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(read, 8u);
  // Page 0 should contain 'A' repeated
  for (size_t i = 0; i < 8; ++i) {
    ASSERT_EQ(buf[i], 'A');
  }
}

TEST_F(CacheClientTest, ReadAcrossPages) {
  // Pages are 16 bytes each. Read 24 bytes starting at offset 8.
  // This spans page 0 (8 bytes from offset 8) and page 1 (full 16 bytes).
  char buf[24];
  size_t read = 0;
  auto s = client_->ReadCached(1, 8, buf, 24, &read);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(read, 24u);

  // First 8 bytes from page 0 ('A')
  for (size_t i = 0; i < 8; ++i) {
    ASSERT_EQ(buf[i], 'A');
  }
  // Next 16 bytes from page 1 ('B')
  for (size_t i = 8; i < 24; ++i) {
    ASSERT_EQ(buf[i], 'B');
  }
}

TEST_F(CacheClientTest, ReadSpansThreePages) {
  // Read 40 bytes starting at offset 4 (spans pages 0, 1, 2)
  // Page 0: bytes 4..15 (12 bytes, 'A')
  // Page 1: bytes 0..15 (16 bytes, 'B')
  // Page 2: bytes 0..11 (12 bytes, 'C')
  char buf[40];
  size_t read = 0;
  auto s = client_->ReadCached(1, 4, buf, 40, &read);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(read, 40u);

  for (size_t i = 0; i < 12; ++i)
    ASSERT_EQ(buf[i], 'A');
  for (size_t i = 12; i < 28; ++i)
    ASSERT_EQ(buf[i], 'B');
  for (size_t i = 28; i < 40; ++i)
    ASSERT_EQ(buf[i], 'C');
}

TEST_F(CacheClientTest, InvalidateFile) {
  // Read to populate cache
  char buf[16];
  size_t read = 0;
  client_->ReadCached(1, 0, buf, 16, &read);
  ASSERT_EQ(read, 16u);

  // Invalidate
  client_->InvalidateFile(1);

  // Read again should still work (fetcher will re-fetch)
  read = 0;
  auto s = client_->ReadCached(1, 0, buf, 16, &read);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(read, 16u);
}
