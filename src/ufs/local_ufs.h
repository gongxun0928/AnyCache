#pragma once

#include "ufs/ufs.h"

#include <mutex>
#include <unordered_map>

namespace anycache {

class LocalUnderFileSystem : public UnderFileSystem {
public:
  explicit LocalUnderFileSystem(const std::string &root_path = "");
  ~LocalUnderFileSystem() override;

  std::string Scheme() const override { return "file"; }

  Status Open(const std::string &path, int flags,
              UfsFileHandle *handle) override;
  Status Create(const std::string &path, const CreateOptions &opts,
                UfsFileHandle *handle) override;
  Status Read(UfsFileHandle handle, void *buf, size_t size, off_t offset,
              size_t *bytes_read) override;
  Status Write(UfsFileHandle handle, const void *buf, size_t size, off_t offset,
               size_t *bytes_written) override;
  Status Close(UfsFileHandle handle) override;

  Status Delete(const std::string &path, bool recursive) override;
  Status Rename(const std::string &src, const std::string &dst) override;
  Status ListDir(const std::string &path,
                 std::vector<UfsFileInfo> *entries) override;
  Status GetFileInfo(const std::string &path, UfsFileInfo *info) override;
  Status Mkdir(const std::string &path, const MkdirOptions &opts) override;
  Status Exists(const std::string &path, bool *exists) override;

private:
  std::string ResolvePath(const std::string &path) const;

  std::string root_path_;
  std::mutex mu_;
  std::unordered_map<UfsFileHandle, int> fd_map_; // handle -> fd
  UfsFileHandle next_handle_ = 1;
};

} // namespace anycache
