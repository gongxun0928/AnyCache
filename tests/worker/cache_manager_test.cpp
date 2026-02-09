#include "worker/cache_manager.h"
#include <gtest/gtest.h>

using namespace anycache;

TEST(CacheManagerTest, LRU_EvictionOrder) {
  CacheManager mgr(CacheManager::PolicyType::kLRU);

  mgr.OnBlockInsert(1, 100);
  mgr.OnBlockInsert(2, 200);
  mgr.OnBlockInsert(3, 300);

  // Access block 1 (moves to end of LRU)
  mgr.OnBlockAccess(1);

  // Evict 200 bytes => should evict block 2 (LRU)
  auto victims = mgr.GetEvictionCandidates(200);
  ASSERT_EQ(victims.size(), 1u);
  EXPECT_EQ(victims[0], 2u);
}

TEST(CacheManagerTest, LRU_EvictsMultiple) {
  CacheManager mgr(CacheManager::PolicyType::kLRU);

  mgr.OnBlockInsert(1, 100);
  mgr.OnBlockInsert(2, 100);
  mgr.OnBlockInsert(3, 100);

  auto victims = mgr.GetEvictionCandidates(250);
  EXPECT_GE(victims.size(), 2u);
}

TEST(CacheManagerTest, LFU_Basic) {
  CacheManager mgr(CacheManager::PolicyType::kLFU);

  mgr.OnBlockInsert(1, 100);
  mgr.OnBlockInsert(2, 100);
  mgr.OnBlockInsert(3, 100);

  // Access block 1 multiple times
  mgr.OnBlockAccess(1);
  mgr.OnBlockAccess(1);
  mgr.OnBlockAccess(1);

  // Access block 3 once
  mgr.OnBlockAccess(3);

  // Block 2 should be evicted first (lowest frequency)
  auto victims = mgr.GetEvictionCandidates(100);
  ASSERT_EQ(victims.size(), 1u);
  EXPECT_EQ(victims[0], 2u);
}

TEST(CacheManagerTest, CachedBytesAccounting) {
  CacheManager mgr(CacheManager::PolicyType::kLRU);

  mgr.OnBlockInsert(1, 100);
  mgr.OnBlockInsert(2, 200);
  EXPECT_EQ(mgr.GetCachedBytes(), 300u);
  EXPECT_EQ(mgr.GetCachedBlockCount(), 2u);

  mgr.OnBlockRemove(1);
  EXPECT_EQ(mgr.GetCachedBytes(), 200u);
  EXPECT_EQ(mgr.GetCachedBlockCount(), 1u);
}
