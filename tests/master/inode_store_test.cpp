#include "master/inode_store.h"
#include <gtest/gtest.h>

#include <filesystem>

using namespace anycache;

// ─── Test fixture ────────────────────────────────────────────────

class InodeStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    db_path_ =
        "/tmp/anycache_test_inode_store_" +
        std::to_string(
            ::testing::UnitTest::GetInstance()->current_test_info()->line());
    std::filesystem::remove_all(db_path_);
    store_ = std::make_unique<InodeStore>();
    ASSERT_TRUE(store_->Open(db_path_).ok());
  }

  void TearDown() override {
    store_->Close();
    store_.reset();
    std::filesystem::remove_all(db_path_);
  }

  // Re-open the store (simulates restart)
  void Reopen() {
    store_->Close();
    store_ = std::make_unique<InodeStore>();
    ASSERT_TRUE(store_->Open(db_path_).ok());
  }

  std::string db_path_;
  std::unique_ptr<InodeStore> store_;
};

// ─── Basic GetInode roundtrip ────────────────────────────────────

TEST_F(InodeStoreTest, PutGetInodeRoundtrip) {
  Inode original;
  original.id = 42;
  original.parent_id = 1;
  original.name = "train.csv";
  original.is_directory = false;
  original.size = 200ULL * 1024 * 1024;
  original.mode = 0644;
  original.owner = "alice";
  original.group = "engineering";
  original.block_size = kDefaultBlockSize;
  original.creation_time_ms = 1700000000000;
  original.modification_time_ms = 1700000001000;
  original.is_complete = true;

  rocksdb::WriteBatch batch;
  store_->BatchPutInode(&batch, original.id, original);
  store_->BatchPutEdge(&batch, original.parent_id, original.name, original.id);
  ASSERT_TRUE(store_->CommitBatch(&batch).ok());

  Inode recovered;
  ASSERT_TRUE(store_->GetInode(42, &recovered).ok());

  EXPECT_EQ(recovered.id, 42u);
  EXPECT_EQ(recovered.parent_id, 1u);
  EXPECT_EQ(recovered.name, "train.csv");
  EXPECT_FALSE(recovered.is_directory);
  EXPECT_EQ(recovered.size, original.size);
  EXPECT_EQ(recovered.mode, 0644u);
  EXPECT_EQ(recovered.owner, "alice");
  EXPECT_EQ(recovered.group, "engineering");
  EXPECT_EQ(recovered.block_size, kDefaultBlockSize);
  EXPECT_EQ(recovered.creation_time_ms, original.creation_time_ms);
  EXPECT_TRUE(recovered.is_complete);
}

TEST_F(InodeStoreTest, GetInodeNotFound) {
  Inode inode;
  auto s = store_->GetInode(999, &inode);
  EXPECT_TRUE(s.IsNotFound());
}

// ─── MultiGetInodes ──────────────────────────────────────────────

TEST_F(InodeStoreTest, MultiGetInodes) {
  // Write 3 file inodes
  for (InodeId id = 10; id <= 12; ++id) {
    Inode inode;
    inode.id = id;
    inode.parent_id = 1;
    inode.name = "file_" + std::to_string(id) + ".txt";
    inode.owner = "bob";
    inode.group = "staff";

    rocksdb::WriteBatch batch;
    store_->BatchPutInode(&batch, id, inode);
    ASSERT_TRUE(store_->CommitBatch(&batch).ok());
  }

  // MultiGet all 3 + 1 non-existent
  std::vector<InodeId> ids = {10, 11, 12, 99};
  std::vector<Inode> results;
  ASSERT_TRUE(store_->MultiGetInodes(ids, &results).ok());

  // Should return 3 (skipping non-existent)
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].name, "file_10.txt");
  EXPECT_EQ(results[1].name, "file_11.txt");
  EXPECT_EQ(results[2].name, "file_12.txt");
  EXPECT_EQ(results[0].owner, "bob");
}

TEST_F(InodeStoreTest, MultiGetEmpty) {
  std::vector<InodeId> ids;
  std::vector<Inode> results;
  ASSERT_TRUE(store_->MultiGetInodes(ids, &results).ok());
  EXPECT_TRUE(results.empty());
}

// ─── DeleteInode ─────────────────────────────────────────────────

TEST_F(InodeStoreTest, DeleteInode) {
  Inode inode;
  inode.id = 5;
  inode.name = "to_delete.txt";

  rocksdb::WriteBatch batch;
  store_->BatchPutInode(&batch, 5, inode);
  ASSERT_TRUE(store_->CommitBatch(&batch).ok());

  // Verify exists
  Inode check;
  ASSERT_TRUE(store_->GetInode(5, &check).ok());

  // Delete
  rocksdb::WriteBatch del_batch;
  store_->BatchDeleteInode(&del_batch, 5);
  ASSERT_TRUE(store_->CommitBatch(&del_batch).ok());

  // Verify gone
  EXPECT_TRUE(store_->GetInode(5, &check).IsNotFound());
}

// ─── ScanDirectoryInodes ─────────────────────────────────────────

TEST_F(InodeStoreTest, ScanDirectoryInodesFiltersCorrectly) {
  // Create 2 directories and 3 files
  auto put_inode = [&](InodeId id, bool is_dir, const std::string &name) {
    Inode inode;
    inode.id = id;
    inode.parent_id = 1;
    inode.name = name;
    inode.is_directory = is_dir;
    inode.mode = is_dir ? 0755u : 0644u;
    rocksdb::WriteBatch batch;
    store_->BatchPutInode(&batch, id, inode);
    ASSERT_TRUE(store_->CommitBatch(&batch).ok());
  };

  put_inode(1, true, "");       // root
  put_inode(2, true, "data");   // directory
  put_inode(3, false, "a.txt"); // file
  put_inode(4, false, "b.txt"); // file
  put_inode(5, false, "c.csv"); // file

  std::vector<Inode> dirs;
  ASSERT_TRUE(store_->ScanDirectoryInodes(&dirs).ok());

  ASSERT_EQ(dirs.size(), 2u);
  // Both should be directories
  for (auto &d : dirs) {
    EXPECT_TRUE(d.is_directory);
  }
}

// ─── ScanAllEdges ────────────────────────────────────────────────

TEST_F(InodeStoreTest, ScanAllEdges) {
  rocksdb::WriteBatch batch;
  store_->BatchPutEdge(&batch, 1, "data", 2);
  store_->BatchPutEdge(&batch, 1, "models", 3);
  store_->BatchPutEdge(&batch, 2, "train.csv", 4);
  store_->BatchPutEdge(&batch, 2, "test.csv", 5);
  ASSERT_TRUE(store_->CommitBatch(&batch).ok());

  std::vector<std::tuple<InodeId, std::string, InodeId>> edges;
  ASSERT_TRUE(store_->ScanAllEdges(&edges).ok());

  ASSERT_EQ(edges.size(), 4u);

  // Verify some expected edges exist
  bool found_data = false, found_train = false;
  for (auto &[pid, name, cid] : edges) {
    if (pid == 1 && name == "data" && cid == 2)
      found_data = true;
    if (pid == 2 && name == "train.csv" && cid == 4)
      found_train = true;
  }
  EXPECT_TRUE(found_data);
  EXPECT_TRUE(found_train);
}

// ─── next_id persistence ─────────────────────────────────────────

TEST_F(InodeStoreTest, NextIdRoundtrip) {
  rocksdb::WriteBatch batch;
  store_->BatchPutNextId(&batch, 12345);
  ASSERT_TRUE(store_->CommitBatch(&batch).ok());

  InodeId next_id = 0;
  ASSERT_TRUE(store_->GetNextId(&next_id).ok());
  EXPECT_EQ(next_id, 12345u);
}

TEST_F(InodeStoreTest, NextIdNotFoundOnFreshDB) {
  InodeId next_id = 0;
  auto s = store_->GetNextId(&next_id);
  EXPECT_TRUE(s.IsNotFound());
}

// ─── OwnerGroupDict persistence across restart ───────────────────

TEST_F(InodeStoreTest, DictPersistsAcrossRestart) {
  // Write inodes with distinct owners/groups
  Inode inode1;
  inode1.id = 10;
  inode1.name = "a.txt";
  inode1.owner = "alice";
  inode1.group = "eng";

  Inode inode2;
  inode2.id = 11;
  inode2.name = "b.txt";
  inode2.owner = "bob";
  inode2.group = "ops";

  {
    rocksdb::WriteBatch batch;
    store_->BatchPutInode(&batch, 10, inode1);
    store_->BatchPutInode(&batch, 11, inode2);
    ASSERT_TRUE(store_->CommitBatch(&batch).ok());
  }

  // Reopen (simulates restart)
  Reopen();

  // Verify owner/group survive restart
  Inode recovered;
  ASSERT_TRUE(store_->GetInode(10, &recovered).ok());
  EXPECT_EQ(recovered.owner, "alice");
  EXPECT_EQ(recovered.group, "eng");

  ASSERT_TRUE(store_->GetInode(11, &recovered).ok());
  EXPECT_EQ(recovered.owner, "bob");
  EXPECT_EQ(recovered.group, "ops");
}

// ─── Edge delete ─────────────────────────────────────────────────

TEST_F(InodeStoreTest, DeleteEdge) {
  rocksdb::WriteBatch batch;
  store_->BatchPutEdge(&batch, 1, "a.txt", 10);
  store_->BatchPutEdge(&batch, 1, "b.txt", 11);
  ASSERT_TRUE(store_->CommitBatch(&batch).ok());

  // Delete one edge
  rocksdb::WriteBatch del_batch;
  store_->BatchDeleteEdge(&del_batch, 1, "a.txt");
  ASSERT_TRUE(store_->CommitBatch(&del_batch).ok());

  // Scan edges: should only have b.txt
  std::vector<std::tuple<InodeId, std::string, InodeId>> edges;
  ASSERT_TRUE(store_->ScanAllEdges(&edges).ok());
  ASSERT_EQ(edges.size(), 1u);
  auto &[pid, name, cid] = edges[0];
  EXPECT_EQ(pid, 1u);
  EXPECT_EQ(name, "b.txt");
  EXPECT_EQ(cid, 11u);
}

// ─── Integration: InodeTree with InodeStore ──────────────────────

class InodeTreeWithStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    db_path_ =
        "/tmp/anycache_test_tree_store_" +
        std::to_string(
            ::testing::UnitTest::GetInstance()->current_test_info()->line());
    std::filesystem::remove_all(db_path_);
    store_ = std::make_unique<InodeStore>();
    ASSERT_TRUE(store_->Open(db_path_).ok());

    tree_ = std::make_unique<InodeTree>();
    tree_->SetStore(store_.get());
    ASSERT_TRUE(tree_->Recover().ok());
  }

  void TearDown() override {
    tree_.reset();
    store_->Close();
    store_.reset();
    std::filesystem::remove_all(db_path_);
  }

  // Simulate restart: destroy tree, reopen store, recover tree
  void Restart() {
    tree_.reset();
    store_->Close();
    store_ = std::make_unique<InodeStore>();
    ASSERT_TRUE(store_->Open(db_path_).ok());
    tree_ = std::make_unique<InodeTree>();
    tree_->SetStore(store_.get());
    ASSERT_TRUE(tree_->Recover().ok());
  }

  std::string db_path_;
  std::unique_ptr<InodeStore> store_;
  std::unique_ptr<InodeTree> tree_;
};

TEST_F(InodeTreeWithStoreTest, RootExistsAfterRecover) {
  Inode root;
  ASSERT_TRUE(tree_->GetInodeByPath("/", &root).ok());
  EXPECT_TRUE(root.is_directory);
  EXPECT_EQ(tree_->DirCount(), 1u); // just root
}

TEST_F(InodeTreeWithStoreTest, CreateFileAndRestart) {
  InodeId file_id;
  ASSERT_TRUE(tree_->CreateFile("/train.csv", 0644, &file_id).ok());

  // File should be accessible
  Inode inode;
  ASSERT_TRUE(tree_->GetInodeByPath("/train.csv", &inode).ok());
  EXPECT_EQ(inode.name, "train.csv");
  EXPECT_FALSE(inode.is_directory);

  // Restart
  Restart();

  // File should still be accessible via RocksDB
  ASSERT_TRUE(tree_->GetInodeByPath("/train.csv", &inode).ok());
  EXPECT_EQ(inode.name, "train.csv");

  // DirCount should be 1 (only root)
  EXPECT_EQ(tree_->DirCount(), 1u);
}

TEST_F(InodeTreeWithStoreTest, CreateDirectoryAndRestart) {
  InodeId dir_id;
  ASSERT_TRUE(tree_->CreateDirectory("/data", 0755, false, &dir_id).ok());

  Restart();

  Inode inode;
  ASSERT_TRUE(tree_->GetInodeByPath("/data", &inode).ok());
  EXPECT_TRUE(inode.is_directory);
  EXPECT_EQ(inode.name, "data");
  EXPECT_EQ(tree_->DirCount(), 2u); // root + data
}

TEST_F(InodeTreeWithStoreTest, RecursiveDirectoryAndRestart) {
  InodeId dir_id;
  ASSERT_TRUE(tree_->CreateDirectory("/a/b/c", 0755, true, &dir_id).ok());

  Restart();

  Inode inode;
  ASSERT_TRUE(tree_->GetInodeByPath("/a/b/c", &inode).ok());
  EXPECT_TRUE(inode.is_directory);
  EXPECT_EQ(tree_->DirCount(), 4u); // root + a + b + c
}

TEST_F(InodeTreeWithStoreTest, ListDirectoryMixed) {
  InodeId id;
  tree_->CreateDirectory("/sub", 0755, false, &id);
  tree_->CreateFile("/a.txt", 0644, &id);
  tree_->CreateFile("/b.txt", 0644, &id);

  std::vector<Inode> children;
  ASSERT_TRUE(tree_->ListDirectory("/", &children).ok());
  EXPECT_EQ(children.size(), 3u);

  // After restart, should still list correctly
  Restart();

  children.clear();
  ASSERT_TRUE(tree_->ListDirectory("/", &children).ok());
  EXPECT_EQ(children.size(), 3u);

  int dirs = 0, files = 0;
  for (auto &c : children) {
    if (c.is_directory)
      dirs++;
    else
      files++;
  }
  EXPECT_EQ(dirs, 1);
  EXPECT_EQ(files, 2);
}

TEST_F(InodeTreeWithStoreTest, CompleteFileAndRestart) {
  InodeId file_id;
  ASSERT_TRUE(tree_->CreateFile("/data.bin", 0644, &file_id).ok());

  uint64_t file_size = 200ULL * 1024 * 1024;
  ASSERT_TRUE(tree_->CompleteFile(file_id, file_size).ok());

  Restart();

  Inode inode;
  ASSERT_TRUE(tree_->GetInodeById(file_id, &inode).ok());
  EXPECT_EQ(inode.size, file_size);
  EXPECT_TRUE(inode.is_complete);
}

TEST_F(InodeTreeWithStoreTest, DeleteFileAndRestart) {
  InodeId id;
  tree_->CreateFile("/del.txt", 0644, &id);
  ASSERT_TRUE(tree_->Delete("/del.txt", false).ok());

  // Should be gone
  Inode inode;
  EXPECT_TRUE(tree_->GetInodeByPath("/del.txt", &inode).IsNotFound());

  Restart();

  // Still gone after restart
  EXPECT_TRUE(tree_->GetInodeByPath("/del.txt", &inode).IsNotFound());
}

TEST_F(InodeTreeWithStoreTest, DeleteDirectoryRecursiveAndRestart) {
  InodeId id;
  tree_->CreateDirectory("/dir", 0755, false, &id);
  tree_->CreateFile("/dir/a.txt", 0644, &id);
  tree_->CreateFile("/dir/b.txt", 0644, &id);

  ASSERT_TRUE(tree_->Delete("/dir", true).ok());

  Inode inode;
  EXPECT_TRUE(tree_->GetInodeByPath("/dir", &inode).IsNotFound());

  Restart();

  EXPECT_TRUE(tree_->GetInodeByPath("/dir", &inode).IsNotFound());
  EXPECT_TRUE(tree_->GetInodeByPath("/dir/a.txt", &inode).IsNotFound());

  // Root should be empty
  std::vector<Inode> children;
  ASSERT_TRUE(tree_->ListDirectory("/", &children).ok());
  EXPECT_EQ(children.size(), 0u);
}

TEST_F(InodeTreeWithStoreTest, RenameFileAndRestart) {
  InodeId id;
  tree_->CreateFile("/old.txt", 0644, &id);

  ASSERT_TRUE(tree_->Rename("/old.txt", "/new.txt").ok());

  Inode inode;
  EXPECT_TRUE(tree_->GetInodeByPath("/old.txt", &inode).IsNotFound());
  ASSERT_TRUE(tree_->GetInodeByPath("/new.txt", &inode).ok());
  EXPECT_EQ(inode.name, "new.txt");

  Restart();

  EXPECT_TRUE(tree_->GetInodeByPath("/old.txt", &inode).IsNotFound());
  ASSERT_TRUE(tree_->GetInodeByPath("/new.txt", &inode).ok());
  EXPECT_EQ(inode.name, "new.txt");
}

TEST_F(InodeTreeWithStoreTest, GetInodeById) {
  InodeId file_id;
  tree_->CreateFile("/test.dat", 0644, &file_id);

  Inode inode;
  ASSERT_TRUE(tree_->GetInodeById(file_id, &inode).ok());
  EXPECT_EQ(inode.name, "test.dat");
  EXPECT_FALSE(inode.is_directory);

  // Directory by id
  InodeId dir_id;
  tree_->CreateDirectory("/mydir", 0755, false, &dir_id);
  ASSERT_TRUE(tree_->GetInodeById(dir_id, &inode).ok());
  EXPECT_EQ(inode.name, "mydir");
  EXPECT_TRUE(inode.is_directory);
}
