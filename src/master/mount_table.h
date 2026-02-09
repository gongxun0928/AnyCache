#pragma once

#include "common/config.h"
#include "common/status.h"
#include "ufs/ufs.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace anycache {

// MountTable maps AnyCache namespace paths to Under File Systems.
// It provides the "unified namespace" feature.
//
// Example:
//   Mount("/data/s3", "s3://my-bucket/prefix")
//   Mount("/data/local", "file:///mnt/storage")
//
// A path lookup on "/data/s3/foo.txt" resolves to the S3 UFS
// with relative path "foo.txt".
//
// Mount points are persisted in RocksDB when Init(db_path) is called.
// After restart, call Init() again to reload from DB.
class MountTable {
public:
  explicit MountTable(const Config &config = Config::Default());
  ~MountTable();

  // Initialize persistence: open RocksDB at db_path and load saved mount
  // points. If not called or db_path empty, mount table is in-memory only (no
  // persist).
  Status Init(const std::string &db_path);

  // Mount a UFS at the given path in AnyCache namespace
  Status Mount(const std::string &anycache_path, const std::string &ufs_uri);

  // Unmount
  Status Unmount(const std::string &anycache_path);

  // Resolve an AnyCache path to its UFS and relative path within it.
  // Returns nullptr if no mount point matches.
  Status Resolve(const std::string &anycache_path, UnderFileSystem **ufs,
                 std::string *relative_path) const;

  // Get all mount points
  std::map<std::string, std::string> GetMountPoints() const;

  // Check if a path is a mount point
  bool IsMountPoint(const std::string &path) const;

private:
  Status LoadFromDb();
  Status PersistMount(const std::string &anycache_path,
                      const std::string &ufs_uri);
  Status PersistUnmount(const std::string &anycache_path);

  struct MountEntry {
    std::string anycache_path;
    std::string ufs_uri;
    std::unique_ptr<UnderFileSystem> ufs;
  };

  Config config_;
  std::string db_path_;

  mutable std::mutex mu_;
  // Ordered by path (longest prefix match)
  std::map<std::string, MountEntry> mounts_;

  // RocksDB for persistence (optional)
  struct MountDb;
  std::unique_ptr<MountDb> db_;
};

} // namespace anycache
