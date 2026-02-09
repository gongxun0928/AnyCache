#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/status.h"
#include "common/types.h"

namespace anycache {

// Options for Create
struct CreateOptions {
  uint32_t mode = 0644;
  bool recursive = false;
};

// Options for Mkdir
struct MkdirOptions {
  uint32_t mode = 0755;
  bool recursive = false;
};

// File info returned by UFS
struct UfsFileInfo {
  std::string name;
  std::string path;
  bool is_directory = false;
  uint64_t size = 0;
  uint32_t mode = 0644;
  std::string owner;
  std::string group;
  int64_t modification_time_ms = 0;
};

// Abstract Under File System interface
class UnderFileSystem {
public:
  virtual ~UnderFileSystem() = default;

  // Get the scheme this UFS handles (e.g., "file", "s3", "hdfs")
  virtual std::string Scheme() const = 0;

  // File operations
  virtual Status Open(const std::string &path, int flags,
                      UfsFileHandle *handle) = 0;
  virtual Status Create(const std::string &path, const CreateOptions &opts,
                        UfsFileHandle *handle) = 0;
  virtual Status Read(UfsFileHandle handle, void *buf, size_t size,
                      off_t offset, size_t *bytes_read) = 0;
  virtual Status Write(UfsFileHandle handle, const void *buf, size_t size,
                       off_t offset, size_t *bytes_written) = 0;
  virtual Status Close(UfsFileHandle handle) = 0;

  // Path operations
  virtual Status Delete(const std::string &path, bool recursive) = 0;
  virtual Status Rename(const std::string &src, const std::string &dst) = 0;
  virtual Status ListDir(const std::string &path,
                         std::vector<UfsFileInfo> *entries) = 0;
  virtual Status GetFileInfo(const std::string &path, UfsFileInfo *info) = 0;
  virtual Status Mkdir(const std::string &path, const MkdirOptions &opts) = 0;
  virtual Status Exists(const std::string &path, bool *exists) = 0;
};

} // namespace anycache
