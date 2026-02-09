#include "worker/block_store.h"
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;
using namespace anycache;

class BlockStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "anycache_bstore_test";
    fs::remove_all(test_dir_);
    fs::create_directories(test_dir_);

    BlockStore::Options opts;
    TierConfig tc;
    tc.type = TierType::kMemory;
    tc.path = "";
    tc.capacity_bytes = 1 * 1024 * 1024; // 1 MB
    opts.tiers.push_back(tc);
    opts.meta_db_path = (test_dir_ / "meta").string();
    opts.cache_policy = CacheManager::PolicyType::kLRU;

    store_ = std::make_unique<BlockStore>(opts);
  }

  void TearDown() override {
    store_.reset();
    fs::remove_all(test_dir_);
  }

  fs::path test_dir_;
  std::unique_ptr<BlockStore> store_;
};

TEST_F(BlockStoreTest, CreateAndReadBlock) {
  // Use composite BlockId: inode_id=10, block_index=0
  BlockId id = MakeBlockId(10, 0);
  ASSERT_TRUE(store_->CreateBlock(id, 4096).ok());

  const char *data = "hello world";
  ASSERT_TRUE(store_->WriteBlock(id, data, strlen(data), 0).ok());

  char buf[64] = {};
  ASSERT_TRUE(store_->ReadBlock(id, buf, strlen(data), 0).ok());
  EXPECT_STREQ(buf, data);
}

TEST_F(BlockStoreTest, RemoveBlock) {
  BlockId id = MakeBlockId(10, 0);
  ASSERT_TRUE(store_->CreateBlock(id, 4096).ok());
  EXPECT_TRUE(store_->HasBlock(id));

  ASSERT_TRUE(store_->RemoveBlock(id).ok());
  EXPECT_FALSE(store_->HasBlock(id));
}

TEST_F(BlockStoreTest, MetaStoreRecovery) {
  BlockId id = MakeBlockId(10, 0);
  ASSERT_TRUE(store_->CreateBlock(id, 4096).ok());

  BlockMeta meta;
  ASSERT_TRUE(store_->GetBlockMeta(id, &meta).ok());
  EXPECT_EQ(meta.block_id, id);
  EXPECT_EQ(meta.length, 4096u);
  // file_id is now derivable from composite BlockId
  EXPECT_EQ(GetInodeId(meta.block_id), 10u);
  EXPECT_EQ(GetBlockIndex(meta.block_id), 0u);
}

TEST_F(BlockStoreTest, EnsureBlockIdempotent) {
  BlockId id = MakeBlockId(10, 0);
  ASSERT_TRUE(store_->EnsureBlock(id, 4096).ok());
  EXPECT_TRUE(store_->HasBlock(id));

  // Calling again should be a no-op
  ASSERT_TRUE(store_->EnsureBlock(id, 4096).ok());
  EXPECT_TRUE(store_->HasBlock(id));
}

TEST_F(BlockStoreTest, EvictionFreesSpace) {
  // Fill the tier with blocks
  std::vector<BlockId> ids;
  for (int i = 0; i < 10; ++i) {
    BlockId id = MakeBlockId(20, i);
    auto s = store_->CreateBlock(id, 100 * 1024); // 100 KB each
    if (s.ok())
      ids.push_back(id);
  }
  EXPECT_GE(ids.size(), 5u); // Should have created several

  // Check total cached bytes
  EXPECT_GT(store_->GetTotalCachedBytes(), 0u);
}
