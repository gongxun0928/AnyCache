#pragma once

#include "common/config.h"
#include "ufs/ufs.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef ANYCACHE_HAS_S3
#include <miniocpp/client.h>
#endif

namespace anycache {

/// S3-compatible Under File System backed by minio-cpp.
/// When ANYCACHE_HAS_S3 is not defined, all methods return NotImplemented.
class S3UnderFileSystem : public UnderFileSystem {
public:
  explicit S3UnderFileSystem(const S3Config &config,
                             const std::string &prefix = "");
  ~S3UnderFileSystem() override;

  std::string Scheme() const override { return "s3"; }

  // File operations
  Status Open(const std::string &path, int flags,
              UfsFileHandle *handle) override;
  Status Create(const std::string &path, const CreateOptions &opts,
                UfsFileHandle *handle) override;
  Status Read(UfsFileHandle handle, void *buf, size_t size, off_t offset,
              size_t *bytes_read) override;
  Status Write(UfsFileHandle handle, const void *buf, size_t size, off_t offset,
               size_t *bytes_written) override;
  Status Close(UfsFileHandle handle) override;

  // Path operations
  Status Delete(const std::string &path, bool recursive) override;
  Status Rename(const std::string &src, const std::string &dst) override;
  Status ListDir(const std::string &path,
                 std::vector<UfsFileInfo> *entries) override;
  Status GetFileInfo(const std::string &path, UfsFileInfo *info) override;
  Status Mkdir(const std::string &path, const MkdirOptions &opts) override;
  Status Exists(const std::string &path, bool *exists) override;

private:
  /// Convert a logical path to an S3 object key, prepending prefix_.
  std::string MakeKey(const std::string &path) const;

  S3Config config_;
  std::string prefix_;

  /// In-memory per-handle state. S3 is not seekable, so we buffer objects
  /// in memory for read/write and flush on Close.
  struct OpenFile {
    std::string key;
    std::vector<char> buffer;
    bool writable = false;
    bool dirty = false;
    bool downloaded = false; // true if buffer was populated from S3
  };

  std::mutex mu_;
  std::unordered_map<UfsFileHandle, OpenFile> open_files_;
  UfsFileHandle next_handle_ = 1;

#ifdef ANYCACHE_HAS_S3
  std::unique_ptr<minio::creds::StaticProvider> creds_provider_;
  std::unique_ptr<minio::s3::Client> client_;
#endif
};

} // namespace anycache
