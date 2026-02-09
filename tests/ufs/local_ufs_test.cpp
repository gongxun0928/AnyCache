#include "ufs/local_ufs.h"
#include <gtest/gtest.h>

#include <cstring>
#include <fcntl.h>
#include <filesystem>

namespace fs = std::filesystem;
using namespace anycache;

class LocalUfsTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "anycache_ufs_test";
    fs::create_directories(test_dir_);
    ufs_ = std::make_unique<LocalUnderFileSystem>(test_dir_.string());
  }

  void TearDown() override {
    ufs_.reset();
    fs::remove_all(test_dir_);
  }

  fs::path test_dir_;
  std::unique_ptr<LocalUnderFileSystem> ufs_;
};

TEST_F(LocalUfsTest, CreateAndRead) {
  UfsFileHandle handle;
  CreateOptions opts;
  ASSERT_TRUE(ufs_->Create("/test.txt", opts, &handle).ok());

  const char *data = "hello, anycache!";
  size_t written;
  ASSERT_TRUE(ufs_->Write(handle, data, strlen(data), 0, &written).ok());
  EXPECT_EQ(written, strlen(data));
  ASSERT_TRUE(ufs_->Close(handle).ok());

  // Read back
  ASSERT_TRUE(ufs_->Open("/test.txt", O_RDONLY, &handle).ok());
  char buf[64] = {};
  size_t read;
  ASSERT_TRUE(ufs_->Read(handle, buf, sizeof(buf), 0, &read).ok());
  EXPECT_EQ(read, strlen(data));
  EXPECT_STREQ(buf, data);
  ASSERT_TRUE(ufs_->Close(handle).ok());
}

TEST_F(LocalUfsTest, MkdirAndListDir) {
  MkdirOptions opts;
  opts.recursive = true;
  ASSERT_TRUE(ufs_->Mkdir("/a/b/c", opts).ok());

  // Create files in /a
  UfsFileHandle handle;
  CreateOptions copts;
  ASSERT_TRUE(ufs_->Create("/a/file1.txt", copts, &handle).ok());
  ufs_->Close(handle);
  ASSERT_TRUE(ufs_->Create("/a/file2.txt", copts, &handle).ok());
  ufs_->Close(handle);

  std::vector<UfsFileInfo> entries;
  ASSERT_TRUE(ufs_->ListDir("/a", &entries).ok());
  EXPECT_GE(entries.size(), 3u); // b, file1.txt, file2.txt
}

TEST_F(LocalUfsTest, DeleteFile) {
  UfsFileHandle handle;
  CreateOptions opts;
  ASSERT_TRUE(ufs_->Create("/todelete.txt", opts, &handle).ok());
  ufs_->Close(handle);

  bool exists;
  ASSERT_TRUE(ufs_->Exists("/todelete.txt", &exists).ok());
  EXPECT_TRUE(exists);

  ASSERT_TRUE(ufs_->Delete("/todelete.txt", false).ok());
  ASSERT_TRUE(ufs_->Exists("/todelete.txt", &exists).ok());
  EXPECT_FALSE(exists);
}

TEST_F(LocalUfsTest, Rename) {
  UfsFileHandle handle;
  CreateOptions opts;
  ASSERT_TRUE(ufs_->Create("/old.txt", opts, &handle).ok());
  const char *data = "rename test";
  size_t written;
  ufs_->Write(handle, data, strlen(data), 0, &written);
  ufs_->Close(handle);

  ASSERT_TRUE(ufs_->Rename("/old.txt", "/new.txt").ok());

  bool exists;
  ufs_->Exists("/old.txt", &exists);
  EXPECT_FALSE(exists);
  ufs_->Exists("/new.txt", &exists);
  EXPECT_TRUE(exists);
}

TEST_F(LocalUfsTest, GetFileInfo) {
  UfsFileHandle handle;
  CreateOptions opts;
  ASSERT_TRUE(ufs_->Create("/info.txt", opts, &handle).ok());
  const char *data = "12345";
  size_t written;
  ufs_->Write(handle, data, 5, 0, &written);
  ufs_->Close(handle);

  UfsFileInfo info;
  ASSERT_TRUE(ufs_->GetFileInfo("/info.txt", &info).ok());
  EXPECT_EQ(info.size, 5u);
  EXPECT_FALSE(info.is_directory);
}

TEST_F(LocalUfsTest, NotFound) {
  UfsFileInfo info;
  Status s = ufs_->GetFileInfo("/nonexistent", &info);
  EXPECT_TRUE(s.IsNotFound());
}
