#include "worker/storage_tier.h"
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;
using namespace anycache;

class StorageTierTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "anycache_tier_test";
    fs::create_directories(test_dir_);
  }
  void TearDown() override { fs::remove_all(test_dir_); }
  fs::path test_dir_;
};

TEST_F(StorageTierTest, MemoryTierAllocateWriteRead) {
  StorageTier tier(TierType::kMemory, "", 1024 * 1024);

  BlockHandle handle;
  ASSERT_TRUE(tier.AllocateBlock(1, 4096, &handle).ok());
  EXPECT_EQ(handle.block_id, 1u);

  const char *data = "hello block";
  ASSERT_TRUE(tier.WriteBlock(1, data, strlen(data), 0).ok());

  char buf[64] = {};
  ASSERT_TRUE(tier.ReadBlock(1, buf, strlen(data), 0).ok());
  EXPECT_STREQ(buf, data);
}

TEST_F(StorageTierTest, MemoryTierCapacityExhausted) {
  StorageTier tier(TierType::kMemory, "", 1024);

  BlockHandle handle;
  ASSERT_TRUE(tier.AllocateBlock(1, 512, &handle).ok());
  ASSERT_TRUE(tier.AllocateBlock(2, 512, &handle).ok());

  Status s = tier.AllocateBlock(3, 512, &handle);
  EXPECT_FALSE(s.ok()); // No space left
}

TEST_F(StorageTierTest, MemoryTierRemove) {
  StorageTier tier(TierType::kMemory, "", 1024);

  BlockHandle handle;
  ASSERT_TRUE(tier.AllocateBlock(1, 512, &handle).ok());
  EXPECT_TRUE(tier.HasBlock(1));

  ASSERT_TRUE(tier.RemoveBlock(1).ok());
  EXPECT_FALSE(tier.HasBlock(1));
  EXPECT_EQ(tier.GetUsedBytes(), 0u);
}

TEST_F(StorageTierTest, DiskTierWriteRead) {
  StorageTier tier(TierType::kSSD, test_dir_.string(), 1024 * 1024);

  BlockHandle handle;
  ASSERT_TRUE(tier.AllocateBlock(1, 4096, &handle).ok());

  const char *data = "disk data";
  ASSERT_TRUE(tier.WriteBlock(1, data, strlen(data), 0).ok());

  char buf[64] = {};
  ASSERT_TRUE(tier.ReadBlock(1, buf, strlen(data), 0).ok());
  EXPECT_STREQ(buf, data);
}

TEST_F(StorageTierTest, ExportImportBlock) {
  StorageTier tier(TierType::kMemory, "", 1024 * 1024);

  BlockHandle handle;
  ASSERT_TRUE(tier.AllocateBlock(1, 64, &handle).ok());
  const char *data = "export test";
  tier.WriteBlock(1, data, strlen(data), 0);

  std::vector<char> exported;
  ASSERT_TRUE(tier.ExportBlock(1, &exported).ok());
  EXPECT_GE(exported.size(), strlen(data));
}
