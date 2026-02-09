#include "master/mount_table.h"
#include "common/logging.h"
#include "ufs/ufs_factory.h"

#include <filesystem>

#include <rocksdb/db.h>

namespace anycache {

struct MountTable::MountDb {
  std::unique_ptr<rocksdb::DB> db;
};

MountTable::MountTable(const Config &config) : config_(config) {
  // Default mount: root -> local FS
  // Users can override via Mount() calls
}

MountTable::~MountTable() = default;

Status MountTable::Init(const std::string &db_path) {
  if (db_path.empty()) {
    return Status::OK();
  }
  std::lock_guard<std::mutex> lock(mu_);
  db_path_ = db_path;
  std::filesystem::create_directories(db_path_);

  rocksdb::Options options;
  options.create_if_missing = true;
  options.compression = rocksdb::kNoCompression;
  rocksdb::DB *raw_db = nullptr;
  auto s = rocksdb::DB::Open(options, db_path_, &raw_db);
  if (!s.ok()) {
    return Status::IOError("MountTable RocksDB open failed: " + s.ToString());
  }
  db_ = std::make_unique<MountDb>();
  db_->db.reset(raw_db);

  RETURN_IF_ERROR(LoadFromDb());
  LOG_INFO("MountTable persistence enabled at {}, loaded {} mount(s)", db_path_,
           mounts_.size());
  return Status::OK();
}

Status MountTable::LoadFromDb() {
  if (!db_ || !db_->db) {
    return Status::OK();
  }
  std::unique_ptr<rocksdb::Iterator> it(
      db_->db->NewIterator(rocksdb::ReadOptions()));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string anycache_path = it->key().ToString();
    std::string ufs_uri = it->value().ToString();
    auto ufs = UfsFactory::Create(ufs_uri, config_);
    if (!ufs) {
      LOG_WARN("MountTable: skip invalid UFS uri at load: {} -> {}",
               anycache_path, ufs_uri);
      continue;
    }
    MountEntry entry;
    entry.anycache_path = anycache_path;
    entry.ufs_uri = ufs_uri;
    entry.ufs = std::move(ufs);
    mounts_[anycache_path] = std::move(entry);
  }
  if (!it->status().ok()) {
    return Status::IOError("MountTable RocksDB scan: " +
                           it->status().ToString());
  }
  return Status::OK();
}

Status MountTable::PersistMount(const std::string &anycache_path,
                                const std::string &ufs_uri) {
  if (!db_ || !db_->db) {
    return Status::OK();
  }
  auto s = db_->db->Put(rocksdb::WriteOptions(), anycache_path, ufs_uri);
  if (!s.ok()) {
    return Status::IOError("MountTable RocksDB put: " + s.ToString());
  }
  return Status::OK();
}

Status MountTable::PersistUnmount(const std::string &anycache_path) {
  if (!db_ || !db_->db) {
    return Status::OK();
  }
  auto s = db_->db->Delete(rocksdb::WriteOptions(), anycache_path);
  if (!s.ok()) {
    return Status::IOError("MountTable RocksDB delete: " + s.ToString());
  }
  return Status::OK();
}

Status MountTable::Mount(const std::string &anycache_path,
                         const std::string &ufs_uri) {
  std::lock_guard<std::mutex> lock(mu_);

  if (mounts_.count(anycache_path)) {
    return Status::AlreadyExists("mount point already exists: " +
                                 anycache_path);
  }

  auto ufs = UfsFactory::Create(ufs_uri, config_);
  if (!ufs) {
    return Status::InvalidArgument("failed to create UFS for: " + ufs_uri);
  }

  Status persist_status = PersistMount(anycache_path, ufs_uri);
  if (!persist_status.ok()) {
    return persist_status;
  }

  MountEntry entry;
  entry.anycache_path = anycache_path;
  entry.ufs_uri = ufs_uri;
  entry.ufs = std::move(ufs);
  mounts_[anycache_path] = std::move(entry);

  LOG_INFO("Mounted {} -> {}", anycache_path, ufs_uri);
  return Status::OK();
}

Status MountTable::Unmount(const std::string &anycache_path) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = mounts_.find(anycache_path);
  if (it == mounts_.end()) {
    return Status::NotFound("mount point not found: " + anycache_path);
  }

  Status persist_status = PersistUnmount(anycache_path);
  if (!persist_status.ok()) {
    return persist_status;
  }

  LOG_INFO("Unmounted {}", anycache_path);
  mounts_.erase(it);
  return Status::OK();
}

Status MountTable::Resolve(const std::string &anycache_path,
                           UnderFileSystem **ufs,
                           std::string *relative_path) const {
  std::lock_guard<std::mutex> lock(mu_);

  // Longest prefix match: iterate in reverse order
  for (auto it = mounts_.rbegin(); it != mounts_.rend(); ++it) {
    const std::string &mount_path = it->first;
    if (anycache_path == mount_path ||
        (anycache_path.size() > mount_path.size() &&
         anycache_path.substr(0, mount_path.size()) == mount_path &&
         (mount_path.back() == '/' ||
          anycache_path[mount_path.size()] == '/'))) {
      *ufs = it->second.ufs.get();
      if (anycache_path.size() > mount_path.size()) {
        size_t start = mount_path.size();
        if (anycache_path[start] == '/')
          start++;
        *relative_path = anycache_path.substr(start);
      } else {
        *relative_path = "";
      }
      return Status::OK();
    }
  }

  return Status::NotFound("no mount point for: " + anycache_path);
}

std::map<std::string, std::string> MountTable::GetMountPoints() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::map<std::string, std::string> result;
  for (auto &[path, entry] : mounts_) {
    result[path] = entry.ufs_uri;
  }
  return result;
}

bool MountTable::IsMountPoint(const std::string &path) const {
  std::lock_guard<std::mutex> lock(mu_);
  return mounts_.count(path) > 0;
}

} // namespace anycache
