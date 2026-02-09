#include "worker/data_mover.h"
#include "worker/worker_service_impl.h"
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;
using namespace anycache;

class WorkerServiceImplTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "anycache_wsvc_test";
    fs::remove_all(test_dir_);
    fs::create_directories(test_dir_);

    BlockStore::Options opts;
    TierConfig tc;
    tc.type = TierType::kMemory;
    tc.path = "";
    tc.capacity_bytes = 4 * 1024 * 1024; // 4 MB
    opts.tiers.push_back(tc);
    opts.meta_db_path = (test_dir_ / "meta").string();
    block_store_ = std::make_unique<BlockStore>(opts);

    page_store_ = std::make_unique<PageStore>(1024 * 1024, 64);

    // DataMover without default UFS (tests use per-task UFS or don't need UFS)
    data_mover_ = std::make_unique<DataMover>(block_store_.get());

    service_ = std::make_unique<WorkerServiceImpl>(block_store_.get(),
                                                   page_store_.get());
    service_->SetDataMover(data_mover_.get());
  }

  void TearDown() override {
    data_mover_->Stop();
    data_mover_.reset();
    service_.reset();
    block_store_.reset();
    page_store_.reset();
    fs::remove_all(test_dir_);
  }

  fs::path test_dir_;
  std::unique_ptr<BlockStore> block_store_;
  std::unique_ptr<PageStore> page_store_;
  std::unique_ptr<DataMover> data_mover_;
  std::unique_ptr<WorkerServiceImpl> service_;
};

TEST_F(WorkerServiceImplTest, WriteAndReadBlock) {
  // Write a block
  proto::WriteBlockRequest write_req;
  write_req.set_block_id(MakeBlockId(1, 0));
  write_req.set_offset(0);
  write_req.set_data("hello world");
  proto::WriteBlockResponse write_resp;

  auto status = service_->WriteBlock(nullptr, &write_req, &write_resp);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(write_resp.status().code(), proto::OK);

  // Read it back
  proto::ReadBlockRequest read_req;
  read_req.set_block_id(MakeBlockId(1, 0));
  read_req.set_offset(0);
  read_req.set_length(11);
  proto::ReadBlockResponse read_resp;

  status = service_->ReadBlock(nullptr, &read_req, &read_resp);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(read_resp.status().code(), proto::OK);
  ASSERT_EQ(read_resp.data(), "hello world");
}

TEST_F(WorkerServiceImplTest, RemoveBlock) {
  // Write a block first
  proto::WriteBlockRequest write_req;
  write_req.set_block_id(MakeBlockId(2, 0));
  write_req.set_offset(0);
  write_req.set_data("data");
  proto::WriteBlockResponse write_resp;
  service_->WriteBlock(nullptr, &write_req, &write_resp);

  // Remove it
  proto::RemoveBlockRequest rm_req;
  rm_req.set_block_id(MakeBlockId(2, 0));
  proto::RemoveBlockResponse rm_resp;
  auto status = service_->RemoveBlock(nullptr, &rm_req, &rm_resp);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(rm_resp.status().code(), proto::OK);

  // Try to read — should fail
  proto::ReadBlockRequest read_req;
  read_req.set_block_id(MakeBlockId(2, 0));
  read_req.set_offset(0);
  read_req.set_length(4);
  proto::ReadBlockResponse read_resp;
  service_->ReadBlock(nullptr, &read_req, &read_resp);
  ASSERT_NE(read_resp.status().code(), proto::OK);
}

TEST_F(WorkerServiceImplTest, AsyncCacheBlockRequiresConfig) {
  // Without config, AsyncCacheBlock should return error
  proto::AsyncCacheBlockRequest req;
  req.set_block_id(MakeBlockId(3, 0));
  req.set_ufs_path("file:///tmp/test");
  req.set_length(100);
  proto::AsyncCacheBlockResponse resp;

  auto status = service_->AsyncCacheBlock(nullptr, &req, &resp);
  ASSERT_TRUE(status.ok());
  // Should fail because config_ is not set
  ASSERT_NE(resp.status().code(), proto::OK);
}

TEST_F(WorkerServiceImplTest, PersistBlockRequiresConfig) {
  // Without config, PersistBlock should return error
  proto::PersistBlockRequest req;
  req.set_block_id(MakeBlockId(4, 0));
  req.set_ufs_path("file:///tmp/test");
  proto::PersistBlockResponse resp;

  auto status = service_->PersistBlock(nullptr, &req, &resp);
  ASSERT_TRUE(status.ok());
  // Should fail because config_ is not set
  ASSERT_NE(resp.status().code(), proto::OK);
}

TEST_F(WorkerServiceImplTest, GetWorkerStatus) {
  proto::GetWorkerStatusRequest req;
  proto::GetWorkerStatusResponse resp;

  auto status = service_->GetWorkerStatus(nullptr, &req, &resp);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(resp.status().code(), proto::OK);
  ASSERT_GT(resp.capacity_bytes(), 0u);
}
