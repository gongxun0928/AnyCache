#include "master/block_master.h"
#include <gtest/gtest.h>

using namespace anycache;

TEST(BlockMasterTest, AddAndGetLocation) {
  BlockMaster bm;
  bm.AddBlockLocation(1, 100, "worker1:29999", TierType::kMemory);
  bm.AddBlockLocation(1, 200, "worker2:29999", TierType::kSSD);

  std::vector<BlockLocationInfo> locs;
  ASSERT_TRUE(bm.GetBlockLocations({1}, &locs).ok());
  EXPECT_EQ(locs.size(), 2u);
}

TEST(BlockMasterTest, RemoveBlockLocation) {
  BlockMaster bm;
  bm.AddBlockLocation(1, 100, "worker1:29999", TierType::kMemory);
  bm.RemoveBlockLocation(1, 100);

  std::vector<BlockLocationInfo> locs;
  bm.GetBlockLocations({1}, &locs);
  EXPECT_EQ(locs.size(), 0u);
}

TEST(BlockMasterTest, RemoveWorkerBlocks) {
  BlockMaster bm;
  bm.AddBlockLocation(1, 100, "w1", TierType::kMemory);
  bm.AddBlockLocation(2, 100, "w1", TierType::kMemory);
  bm.AddBlockLocation(3, 200, "w2", TierType::kMemory);

  bm.RemoveWorkerBlocks(100);

  EXPECT_EQ(bm.GetReplicaCount(1), 0u);
  EXPECT_EQ(bm.GetReplicaCount(2), 0u);
  EXPECT_EQ(bm.GetReplicaCount(3), 1u);
}

TEST(BlockMasterTest, CompositeBlockId) {
  // Block IDs are now computed, not allocated
  BlockId bid = MakeBlockId(42, 3);
  EXPECT_EQ(GetInodeId(bid), 42u);
  EXPECT_EQ(GetBlockIndex(bid), 3u);
}

TEST(BlockMasterTest, GetWorkerBlocks) {
  BlockMaster bm;
  bm.AddBlockLocation(10, 1, "w1", TierType::kSSD);
  bm.AddBlockLocation(20, 1, "w1", TierType::kSSD);

  auto blocks = bm.GetWorkerBlocks(1);
  EXPECT_EQ(blocks.size(), 2u);
}
