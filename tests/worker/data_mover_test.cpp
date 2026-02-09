#include "ufs/local_ufs.h"
#include "worker/data_mover.h"
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace anycache;

class DataMoverTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "anycache_datamover_test";
    ufs_dir_ = test_dir_ / "ufs";
    fs::remove_all(test_dir_);
    fs::create_directories(ufs_dir_);

    BlockStore::Options opts;
    TierConfig tc;
    tc.type = TierType::kMemory;
    tc.path = "";
    tc.capacity_bytes = 4 * 1024 * 1024;
    opts.tiers.push_back(tc);
    opts.meta_db_path = (test_dir_ / "meta").string();
    block_store_ = std::make_unique<BlockStore>(opts);

    ufs_ = std::make_unique<LocalUnderFileSystem>(ufs_dir_.string());
  }

  void TearDown() override {
    block_store_.reset();
    ufs_.reset();
    fs::remove_all(test_dir_);
  }

  // Helper: write a file in the UFS directory
  void WriteUfsFile(const std::string &name, const std::string &content) {
    std::ofstream ofs((ufs_dir_ / name).string(), std::ios::binary);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  fs::path test_dir_;
  fs::path ufs_dir_;
  std::unique_ptr<BlockStore> block_store_;
  std::unique_ptr<LocalUnderFileSystem> ufs_;
};

TEST_F(DataMoverTest, PreloadFromUFS) {
  // Write a file to UFS
  WriteUfsFile("testfile.dat", "preload-data-12345");

  DataMover mover(block_store_.get(), ufs_.get(), 2);

  BlockId bid = MakeBlockId(1, 0);
  auto s = mover.SubmitPreload(bid, "testfile.dat", 0, 18);
  ASSERT_TRUE(s.ok());

  mover.WaitAll();

  // Verify block was cached
  ASSERT_TRUE(block_store_->HasBlock(bid));
  char buf[32] = {};
  s = block_store_->ReadBlock(bid, buf, 18, 0);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(std::string(buf, 18), "preload-data-12345");

  mover.Stop();
}

TEST_F(DataMoverTest, PersistToUFS) {
  // Create a block in cache
  BlockId bid = MakeBlockId(2, 0);
  block_store_->CreateBlock(bid, 10);
  block_store_->WriteBlock(bid, "persist-me", 10, 0);

  DataMover mover(block_store_.get(), ufs_.get(), 2);

  auto s = mover.SubmitPersist(bid, "output.dat", 0);
  ASSERT_TRUE(s.ok());

  mover.WaitAll();

  // Verify the file was written to UFS
  auto path = ufs_dir_ / "output.dat";
  ASSERT_TRUE(fs::exists(path));
  std::ifstream ifs(path.string(), std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  ASSERT_EQ(content, "persist-me");

  mover.Stop();
}

TEST_F(DataMoverTest, PreloadWithPerTaskUFS) {
  // Write a file to UFS
  WriteUfsFile("pertask.dat", "task-specific-data");

  // Create DataMover without default UFS
  DataMover mover(block_store_.get(), 2);

  // Use per-task UFS
  auto task_ufs = std::make_shared<LocalUnderFileSystem>(ufs_dir_.string());

  BlockId bid = MakeBlockId(3, 0);
  auto s = mover.SubmitPreload(bid, "pertask.dat", 0, 18, task_ufs);
  ASSERT_TRUE(s.ok());

  mover.WaitAll();

  ASSERT_TRUE(block_store_->HasBlock(bid));
  char buf[32] = {};
  s = block_store_->ReadBlock(bid, buf, 18, 0);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(std::string(buf, 18), "task-specific-data");

  mover.Stop();
}

TEST_F(DataMoverTest, NoUFSAvailableReturnsError) {
  // DataMover without UFS, submit without per-task UFS
  DataMover mover(block_store_.get(), 1);

  BlockId bid = MakeBlockId(4, 0);
  auto s = mover.SubmitPreload(bid, "nofile.dat", 0, 10);
  ASSERT_TRUE(s.ok()); // Submit succeeds

  mover.WaitAll();

  // The block should NOT exist (task failed due to no UFS)
  ASSERT_FALSE(block_store_->HasBlock(bid));

  mover.Stop();
}
