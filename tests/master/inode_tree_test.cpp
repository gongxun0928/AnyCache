#include "master/inode_tree.h"
#include <gtest/gtest.h>

using namespace anycache;

class InodeTreeTest : public ::testing::Test {
protected:
  InodeTree tree;
};

TEST_F(InodeTreeTest, RootExists) {
  Inode root;
  ASSERT_TRUE(tree.GetInodeByPath("/", &root).ok());
  EXPECT_TRUE(root.is_directory);
}

TEST_F(InodeTreeTest, CreateFileAtRoot) {
  InodeId id;
  ASSERT_TRUE(tree.CreateFile("/test.txt", 0644, &id).ok());
  EXPECT_NE(id, kInvalidInodeId);

  Inode inode;
  ASSERT_TRUE(tree.GetInodeByPath("/test.txt", &inode).ok());
  EXPECT_EQ(inode.name, "test.txt");
  EXPECT_FALSE(inode.is_directory);
}

TEST_F(InodeTreeTest, CreateDirectory) {
  InodeId id;
  ASSERT_TRUE(tree.CreateDirectory("/data", 0755, false, &id).ok());

  Inode inode;
  ASSERT_TRUE(tree.GetInodeByPath("/data", &inode).ok());
  EXPECT_TRUE(inode.is_directory);
}

TEST_F(InodeTreeTest, CreateDirectoryRecursive) {
  InodeId id;
  ASSERT_TRUE(tree.CreateDirectory("/a/b/c", 0755, true, &id).ok());

  Inode inode;
  ASSERT_TRUE(tree.GetInodeByPath("/a/b/c", &inode).ok());
  EXPECT_TRUE(inode.is_directory);
}

TEST_F(InodeTreeTest, CreateFileInSubdir) {
  InodeId dir_id;
  tree.CreateDirectory("/dir", 0755, false, &dir_id);

  InodeId file_id;
  ASSERT_TRUE(tree.CreateFile("/dir/file.txt", 0644, &file_id).ok());

  Inode inode;
  ASSERT_TRUE(tree.GetInodeByPath("/dir/file.txt", &inode).ok());
  EXPECT_EQ(inode.name, "file.txt");
}

TEST_F(InodeTreeTest, DuplicateFileError) {
  InodeId id;
  ASSERT_TRUE(tree.CreateFile("/dup.txt", 0644, &id).ok());
  Status s = tree.CreateFile("/dup.txt", 0644, &id);
  EXPECT_TRUE(s.IsAlreadyExists());
}

TEST_F(InodeTreeTest, DeleteFile) {
  InodeId id;
  tree.CreateFile("/todel.txt", 0644, &id);
  ASSERT_TRUE(tree.Delete("/todel.txt", false).ok());

  Inode inode;
  EXPECT_TRUE(tree.GetInodeByPath("/todel.txt", &inode).IsNotFound());
}

TEST_F(InodeTreeTest, RenameFile) {
  InodeId id;
  tree.CreateFile("/old.txt", 0644, &id);
  ASSERT_TRUE(tree.Rename("/old.txt", "/new.txt").ok());

  Inode inode;
  EXPECT_TRUE(tree.GetInodeByPath("/old.txt", &inode).IsNotFound());
  ASSERT_TRUE(tree.GetInodeByPath("/new.txt", &inode).ok());
  EXPECT_EQ(inode.name, "new.txt");
}

TEST_F(InodeTreeTest, ListDirectory) {
  InodeId id;
  tree.CreateFile("/a.txt", 0644, &id);
  tree.CreateFile("/b.txt", 0644, &id);
  tree.CreateDirectory("/sub", 0755, false, &id);

  std::vector<Inode> children;
  ASSERT_TRUE(tree.ListDirectory("/", &children).ok());
  EXPECT_EQ(children.size(), 3u);
}

TEST_F(InodeTreeTest, CompleteFile) {
  InodeId id;
  tree.CreateFile("/data.bin", 0644, &id);

  // 200 MB file = 4 blocks at default 64MB block size
  uint64_t file_size = 200ULL * 1024 * 1024;
  ASSERT_TRUE(tree.CompleteFile(id, file_size).ok());

  Inode inode;
  ASSERT_TRUE(tree.GetInodeById(id, &inode).ok());
  EXPECT_EQ(inode.size, file_size);
  EXPECT_TRUE(inode.is_complete);

  // Verify blocks are derivable from composite BlockId
  uint32_t block_count = GetBlockCount(inode.size, inode.block_size);
  EXPECT_EQ(block_count, 4u);
  for (uint32_t i = 0; i < block_count; ++i) {
    BlockId bid = MakeBlockId(id, i);
    EXPECT_EQ(GetInodeId(bid), id);
    EXPECT_EQ(GetBlockIndex(bid), i);
  }
}
