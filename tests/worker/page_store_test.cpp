#include "worker/page_store.h"
#include <gtest/gtest.h>

#include <cstring>

using namespace anycache;

TEST(PageStoreTest, WriteAndRead) {
  PageStore store(4096, 100);

  const char *data = "page data test";
  ASSERT_TRUE(store.WritePage(1, 0, data, strlen(data)).ok());

  // Read back directly from cache (no fetcher needed)
  char buf[4096] = {};
  size_t bytes_read = 0;
  // The page is already cached via WritePage
  std::shared_ptr<PageData> dummy;
  // Use the store's internal cache
  ASSERT_TRUE(store.ReadPage(1, 0, buf, &bytes_read).ok());
  EXPECT_EQ(bytes_read, strlen(data));
  EXPECT_STREQ(buf, data);
}

TEST(PageStoreTest, CacheMissWithFetcher) {
  PageStore store(64, 100);

  // Set up a fetcher that returns fixed data
  store.SetPageFetcher([](FileId file_id, uint64_t page_index, void *buf,
                          size_t *bytes_read) -> Status {
    std::string data = "fetched_page_" + std::to_string(page_index);
    std::memcpy(buf, data.data(), data.size());
    *bytes_read = data.size();
    return Status::OK();
  });

  char buf[64] = {};
  size_t bytes_read = 0;
  ASSERT_TRUE(store.ReadPage(42, 5, buf, &bytes_read).ok());
  EXPECT_GT(bytes_read, 0u);
  EXPECT_EQ(std::string(buf, bytes_read), "fetched_page_5");
}

TEST(PageStoreTest, CacheMissNoFetcher) {
  PageStore store(64, 100);
  char buf[64];
  size_t bytes_read;
  Status s = store.ReadPage(1, 0, buf, &bytes_read);
  EXPECT_FALSE(s.ok()); // Should fail without fetcher
}

TEST(PageStoreTest, EvictPages) {
  PageStore store(64, 5);

  // Fill cache with 5 pages
  for (uint64_t i = 0; i < 5; ++i) {
    char data[64];
    snprintf(data, sizeof(data), "page_%llu", i);
    store.WritePage(1, i, data, strlen(data));
  }
  EXPECT_EQ(store.GetCachedPageCount(), 5u);

  store.Evict(3);
  EXPECT_EQ(store.GetCachedPageCount(), 2u);
}

TEST(PageStoreTest, Prefetch) {
  PageStore store(64, 100);
  store.SetPageFetcher(
      [](FileId, uint64_t page_index, void *buf, size_t *bytes_read) -> Status {
        std::string data = "p" + std::to_string(page_index);
        std::memcpy(buf, data.data(), data.size());
        *bytes_read = data.size();
        return Status::OK();
      });

  store.PrefetchPages(1, 10, 5);
  EXPECT_EQ(store.GetCachedPageCount(), 5u);
}
