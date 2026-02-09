#include "master/mount_table.h"
#include <gtest/gtest.h>

#include <filesystem>

namespace fs = std::filesystem;
using namespace anycache;

class MountTableTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "anycache_mount_test";
    fs::create_directories(test_dir_ / "local1");
    fs::create_directories(test_dir_ / "local2");
  }
  void TearDown() override { fs::remove_all(test_dir_); }
  fs::path test_dir_;
};

TEST_F(MountTableTest, MountAndResolve) {
  MountTable mt;

  std::string uri1 = "file://" + (test_dir_ / "local1").string();
  std::string uri2 = "file://" + (test_dir_ / "local2").string();

  ASSERT_TRUE(mt.Mount("/data/a", uri1).ok());
  ASSERT_TRUE(mt.Mount("/data/b", uri2).ok());

  UnderFileSystem *ufs = nullptr;
  std::string rel;

  ASSERT_TRUE(mt.Resolve("/data/a/foo.txt", &ufs, &rel).ok());
  EXPECT_NE(ufs, nullptr);
  EXPECT_EQ(rel, "foo.txt");

  ASSERT_TRUE(mt.Resolve("/data/b/sub/bar.txt", &ufs, &rel).ok());
  EXPECT_EQ(rel, "sub/bar.txt");
}

TEST_F(MountTableTest, DuplicateMount) {
  MountTable mt;
  std::string uri = "file://" + (test_dir_ / "local1").string();
  ASSERT_TRUE(mt.Mount("/data", uri).ok());
  Status s = mt.Mount("/data", uri);
  EXPECT_TRUE(s.IsAlreadyExists());
}

TEST_F(MountTableTest, Unmount) {
  MountTable mt;
  std::string uri = "file://" + (test_dir_ / "local1").string();
  ASSERT_TRUE(mt.Mount("/data", uri).ok());
  ASSERT_TRUE(mt.Unmount("/data").ok());

  UnderFileSystem *ufs;
  std::string rel;
  Status s = mt.Resolve("/data/foo", &ufs, &rel);
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(MountTableTest, NoMatchingMount) {
  MountTable mt;
  UnderFileSystem *ufs;
  std::string rel;
  Status s = mt.Resolve("/unmounted/path", &ufs, &rel);
  EXPECT_TRUE(s.IsNotFound());
}

TEST_F(MountTableTest, GetMountPoints) {
  MountTable mt;
  std::string uri1 = "file://" + (test_dir_ / "local1").string();
  std::string uri2 = "file://" + (test_dir_ / "local2").string();
  mt.Mount("/a", uri1);
  mt.Mount("/b", uri2);

  auto mounts = mt.GetMountPoints();
  EXPECT_EQ(mounts.size(), 2u);
  EXPECT_TRUE(mounts.count("/a"));
  EXPECT_TRUE(mounts.count("/b"));
}

TEST_F(MountTableTest, PersistAndRecover) {
  std::string db_path = (test_dir_ / "mount_db").string();
  std::string uri = "file://" + (test_dir_ / "local1").string();

  {
    MountTable mt;
    ASSERT_TRUE(mt.Init(db_path).ok());
    ASSERT_TRUE(mt.Mount("/persisted", uri).ok());
    auto mounts = mt.GetMountPoints();
    EXPECT_EQ(mounts.size(), 1u);
    EXPECT_EQ(mounts["/persisted"], uri);
  }

  {
    MountTable mt;
    ASSERT_TRUE(mt.Init(db_path).ok());
    auto mounts = mt.GetMountPoints();
    EXPECT_EQ(mounts.size(), 1u);
    EXPECT_EQ(mounts["/persisted"], uri);
    UnderFileSystem *ufs = nullptr;
    std::string rel;
    ASSERT_TRUE(mt.Resolve("/persisted/foo.txt", &ufs, &rel).ok());
    EXPECT_NE(ufs, nullptr);
    EXPECT_EQ(rel, "foo.txt");
  }
}
