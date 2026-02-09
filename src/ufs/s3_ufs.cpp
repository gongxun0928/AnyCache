#include "ufs/s3_ufs.h"
#include "common/logging.h"

#include <cstring>

#ifdef ANYCACHE_HAS_S3
#include <miniocpp/client.h>
#include <sstream>
#endif

namespace anycache {

// ─── Constructor / Destructor ────────────────────────────────

S3UnderFileSystem::S3UnderFileSystem(const S3Config &config,
                                     const std::string &prefix)
    : config_(config), prefix_(prefix) {
#ifdef ANYCACHE_HAS_S3
  // Build the base URL from endpoint.
  // minio::s3::BaseUrl expects host[:port] without scheme prefix.
  // If the endpoint starts with "http://" or "https://", we extract it.
  std::string endpoint = config_.endpoint;
  bool use_ssl = true;
  if (endpoint.starts_with("http://")) {
    endpoint = endpoint.substr(7);
    use_ssl = false;
  } else if (endpoint.starts_with("https://")) {
    endpoint = endpoint.substr(8);
  }

  minio::s3::BaseUrl base_url(endpoint, use_ssl);

  // Credential provider
  creds_provider_ = std::make_unique<minio::creds::StaticProvider>(
      config_.access_key, config_.secret_key);

  // Create client
  client_ =
      std::make_unique<minio::s3::Client>(base_url, creds_provider_.get());
#endif
  LOG_INFO("S3UnderFileSystem: bucket={}, prefix={}, endpoint={}",
           config_.bucket, prefix_, config_.endpoint);
}

S3UnderFileSystem::~S3UnderFileSystem() {
  std::lock_guard<std::mutex> lock(mu_);
  open_files_.clear();
}

// ─── Helpers ─────────────────────────────────────────────────

std::string S3UnderFileSystem::MakeKey(const std::string &path) const {
  std::string key = prefix_;
  if (!key.empty() && key.back() != '/' && !path.empty() &&
      path.front() != '/') {
    key += '/';
  }
  key += path;
  // Remove leading slash
  while (!key.empty() && key.front() == '/') {
    key = key.substr(1);
  }
  return key;
}

// ─── File operations ─────────────────────────────────────────

Status S3UnderFileSystem::Open(const std::string &path, int /*flags*/,
                               UfsFileHandle *handle) {
#ifdef ANYCACHE_HAS_S3
  std::string key = MakeKey(path);

  // Verify object exists via StatObject
  minio::s3::StatObjectArgs stat_args;
  stat_args.bucket = config_.bucket;
  stat_args.object = key;
  auto stat_resp = client_->StatObject(stat_args);
  if (!stat_resp) {
    return Status::NotFound("S3 object not found: " + key + " (" +
                            stat_resp.Error().String() + ")");
  }

  std::lock_guard<std::mutex> lock(mu_);
  *handle = next_handle_++;
  OpenFile of;
  of.key = key;
  of.writable = false;
  of.downloaded = false;
  open_files_[*handle] = std::move(of);
  return Status::OK();
#else
  (void)path;
  (void)handle;
  return Status::NotImplemented(
      "S3 support not compiled (enable ANYCACHE_ENABLE_S3)");
#endif
}

Status S3UnderFileSystem::Create(const std::string &path,
                                 const CreateOptions & /*opts*/,
                                 UfsFileHandle *handle) {
#ifdef ANYCACHE_HAS_S3
  std::string key = MakeKey(path);

  std::lock_guard<std::mutex> lock(mu_);
  *handle = next_handle_++;
  OpenFile of;
  of.key = key;
  of.writable = true;
  of.downloaded = true; // no need to download, starts empty
  open_files_[*handle] = std::move(of);
  return Status::OK();
#else
  (void)path;
  (void)handle;
  return Status::NotImplemented("S3 support not compiled");
#endif
}

Status S3UnderFileSystem::Read(UfsFileHandle handle, void *buf, size_t size,
                               off_t offset, size_t *bytes_read) {
#ifdef ANYCACHE_HAS_S3
  std::lock_guard<std::mutex> lock(mu_);
  auto it = open_files_.find(handle);
  if (it == open_files_.end()) {
    return Status::InvalidArgument("invalid handle");
  }

  auto &of = it->second;

  // Lazy download: fetch object from S3 on first read
  if (!of.downloaded) {
    of.buffer.clear();
    minio::s3::GetObjectArgs get_args;
    get_args.bucket = config_.bucket;
    get_args.object = of.key;
    get_args.datafunc = [&of](minio::http::DataFunctionArgs args) -> bool {
      of.buffer.insert(of.buffer.end(), args.datachunk.begin(),
                       args.datachunk.end());
      return true;
    };

    auto resp = client_->GetObject(get_args);
    if (!resp) {
      return Status::IOError("S3 GetObject failed for " + of.key + ": " +
                             resp.Error().String());
    }
    of.downloaded = true;
  }

  // Read from in-memory buffer
  size_t buf_size = of.buffer.size();
  size_t off = static_cast<size_t>(offset);
  if (off >= buf_size) {
    *bytes_read = 0;
    return Status::OK();
  }
  size_t avail = buf_size - off;
  size_t to_read = std::min(size, avail);
  std::memcpy(buf, of.buffer.data() + off, to_read);
  *bytes_read = to_read;
  return Status::OK();
#else
  (void)handle;
  (void)buf;
  (void)size;
  (void)offset;
  (void)bytes_read;
  return Status::NotImplemented("S3 support not compiled");
#endif
}

Status S3UnderFileSystem::Write(UfsFileHandle handle, const void *buf,
                                size_t size, off_t offset,
                                size_t *bytes_written) {
#ifdef ANYCACHE_HAS_S3
  std::lock_guard<std::mutex> lock(mu_);
  auto it = open_files_.find(handle);
  if (it == open_files_.end()) {
    return Status::InvalidArgument("invalid handle");
  }

  auto &of = it->second;
  if (!of.writable) {
    return Status::PermissionDenied("file not opened for writing");
  }

  size_t needed = static_cast<size_t>(offset) + size;
  if (of.buffer.size() < needed) {
    of.buffer.resize(needed);
  }
  std::memcpy(of.buffer.data() + offset, buf, size);
  of.dirty = true;
  *bytes_written = size;
  return Status::OK();
#else
  (void)handle;
  (void)buf;
  (void)size;
  (void)offset;
  (void)bytes_written;
  return Status::NotImplemented("S3 support not compiled");
#endif
}

Status S3UnderFileSystem::Close(UfsFileHandle handle) {
#ifdef ANYCACHE_HAS_S3
  std::lock_guard<std::mutex> lock(mu_);
  auto it = open_files_.find(handle);
  if (it == open_files_.end()) {
    return Status::InvalidArgument("invalid handle");
  }

  // If dirty, upload the buffer to S3
  if (it->second.dirty) {
    auto &of = it->second;
    std::string data(of.buffer.begin(), of.buffer.end());
    std::istringstream stream(data);
    minio::s3::PutObjectArgs put_args(stream,
                                      static_cast<long>(of.buffer.size()), 0);
    put_args.bucket = config_.bucket;
    put_args.object = of.key;

    auto resp = client_->PutObject(put_args);
    if (!resp) {
      std::string err_msg =
          "S3 PutObject failed for " + of.key + ": " + resp.Error().String();
      open_files_.erase(it);
      return Status::IOError(err_msg);
    }
    LOG_DEBUG("S3: uploaded {} bytes to s3://{}/{}", of.buffer.size(),
              config_.bucket, of.key);
  }

  open_files_.erase(it);
  return Status::OK();
#else
  (void)handle;
  return Status::NotImplemented("S3 support not compiled");
#endif
}

// ─── Path operations ─────────────────────────────────────────

Status S3UnderFileSystem::Delete(const std::string &path, bool recursive) {
#ifdef ANYCACHE_HAS_S3
  std::string key = MakeKey(path);

  if (recursive) {
    // List all objects with this prefix and delete them
    std::string prefix = key;
    if (!prefix.empty() && prefix.back() != '/') {
      prefix += '/';
    }

    minio::s3::ListObjectsArgs list_args;
    list_args.bucket = config_.bucket;
    list_args.prefix = prefix;
    list_args.recursive = true;

    auto result = client_->ListObjects(list_args);
    for (; result; result++) {
      minio::s3::Item item = *result;
      if (!item) {
        return Status::IOError("S3 ListObjects failed: " +
                               item.Error().String());
      }
      minio::s3::RemoveObjectArgs rm_args;
      rm_args.bucket = config_.bucket;
      rm_args.object = item.name;
      auto rm_resp = client_->RemoveObject(rm_args);
      if (!rm_resp) {
        return Status::IOError("S3 RemoveObject failed for " + item.name +
                               ": " + rm_resp.Error().String());
      }
    }
  }

  // Delete the object itself (may be a "file" or a "directory marker")
  minio::s3::RemoveObjectArgs rm_args;
  rm_args.bucket = config_.bucket;
  rm_args.object = key;
  auto resp = client_->RemoveObject(rm_args);
  // S3 RemoveObject is idempotent; not finding the object is OK
  if (!resp) {
    LOG_DEBUG("S3 RemoveObject for {} returned: {}", key,
              resp.Error().String());
  }
  return Status::OK();
#else
  (void)path;
  (void)recursive;
  return Status::NotImplemented("S3 support not compiled");
#endif
}

Status S3UnderFileSystem::Rename(const std::string &src,
                                 const std::string &dst) {
#ifdef ANYCACHE_HAS_S3
  // S3 does not support rename natively. We do copy + delete.
  std::string src_key = MakeKey(src);
  std::string dst_key = MakeKey(dst);

  minio::s3::CopyObjectArgs copy_args;
  copy_args.bucket = config_.bucket;
  copy_args.object = dst_key;
  minio::s3::CopySource source;
  source.bucket = config_.bucket;
  source.object = src_key;
  copy_args.source = source;

  auto copy_resp = client_->CopyObject(copy_args);
  if (!copy_resp) {
    return Status::IOError("S3 CopyObject failed (" + src_key + " -> " +
                           dst_key + "): " + copy_resp.Error().String());
  }

  // Delete the source
  minio::s3::RemoveObjectArgs rm_args;
  rm_args.bucket = config_.bucket;
  rm_args.object = src_key;
  auto rm_resp = client_->RemoveObject(rm_args);
  if (!rm_resp) {
    return Status::IOError("S3 RemoveObject failed for " + src_key + ": " +
                           rm_resp.Error().String());
  }

  return Status::OK();
#else
  (void)src;
  (void)dst;
  return Status::NotImplemented("S3 support not compiled");
#endif
}

Status S3UnderFileSystem::ListDir(const std::string &path,
                                  std::vector<UfsFileInfo> *entries) {
#ifdef ANYCACHE_HAS_S3
  std::string prefix = MakeKey(path);
  if (!prefix.empty() && prefix.back() != '/') {
    prefix += '/';
  }

  minio::s3::ListObjectsArgs list_args;
  list_args.bucket = config_.bucket;
  list_args.prefix = prefix;
  list_args.recursive = false; // single level

  auto result = client_->ListObjects(list_args);
  for (; result; result++) {
    minio::s3::Item item = *result;
    if (!item) {
      return Status::IOError("S3 ListObjects failed: " + item.Error().String());
    }

    UfsFileInfo info;
    if (item.is_prefix) {
      // "Directory" (common prefix)
      info.name = item.name;
      // Remove trailing slash for display and remove prefix
      if (!info.name.empty() && info.name.back() == '/') {
        info.name.pop_back();
      }
      // Extract just the last component
      auto slash = info.name.rfind('/');
      if (slash != std::string::npos) {
        info.name = info.name.substr(slash + 1);
      }
      info.path = item.name;
      info.is_directory = true;
      info.size = 0;
    } else {
      // Regular object
      info.name = item.name;
      // Extract just the filename part (after the prefix)
      if (info.name.size() > prefix.size()) {
        info.name = info.name.substr(prefix.size());
      }
      info.path = item.name;
      info.is_directory = false;
      info.size = static_cast<uint64_t>(item.size);
      // Convert last_modified to epoch ms
      // minio::utils::UtcTime has ToUTC() that returns a time_point
    }
    entries->push_back(std::move(info));
  }
  return Status::OK();
#else
  (void)path;
  (void)entries;
  return Status::NotImplemented("S3 support not compiled");
#endif
}

Status S3UnderFileSystem::GetFileInfo(const std::string &path,
                                      UfsFileInfo *info) {
#ifdef ANYCACHE_HAS_S3
  std::string key = MakeKey(path);

  minio::s3::StatObjectArgs stat_args;
  stat_args.bucket = config_.bucket;
  stat_args.object = key;

  auto resp = client_->StatObject(stat_args);
  if (resp) {
    // Object exists
    info->name = path;
    auto slash = info->name.rfind('/');
    if (slash != std::string::npos) {
      info->name = info->name.substr(slash + 1);
    }
    info->path = key;
    info->is_directory = false;
    info->size = static_cast<uint64_t>(resp.size);
    info->mode = 0644;
    return Status::OK();
  }

  // Maybe it's a "directory" (prefix)? Check if any objects exist with key as
  // prefix.
  std::string prefix = key;
  if (!prefix.empty() && prefix.back() != '/') {
    prefix += '/';
  }

  minio::s3::ListObjectsArgs list_args;
  list_args.bucket = config_.bucket;
  list_args.prefix = prefix;
  list_args.recursive = false;

  auto result = client_->ListObjects(list_args);
  if (result) {
    minio::s3::Item item = *result;
    if (item) {
      // Has at least one child → treat as directory
      info->name = path;
      auto slash2 = info->name.rfind('/');
      if (slash2 != std::string::npos) {
        info->name = info->name.substr(slash2 + 1);
      }
      info->path = key;
      info->is_directory = true;
      info->size = 0;
      info->mode = 0755;
      return Status::OK();
    }
  }

  return Status::NotFound("S3 object not found: " + key);
#else
  (void)path;
  (void)info;
  return Status::NotImplemented("S3 support not compiled");
#endif
}

Status S3UnderFileSystem::Mkdir(const std::string & /*path*/,
                                const MkdirOptions & /*opts*/) {
  // S3 has no real directories. Optionally we could create a zero-byte
  // "directory marker" object with trailing slash, but most S3-compatible
  // systems don't require it. No-op is the simplest correct behavior.
  return Status::OK();
}

Status S3UnderFileSystem::Exists(const std::string &path, bool *exists) {
#ifdef ANYCACHE_HAS_S3
  std::string key = MakeKey(path);

  // Check if object itself exists
  minio::s3::StatObjectArgs stat_args;
  stat_args.bucket = config_.bucket;
  stat_args.object = key;

  auto resp = client_->StatObject(stat_args);
  if (resp) {
    *exists = true;
    return Status::OK();
  }

  // Check if any object with this prefix exists (directory semantics)
  std::string prefix = key;
  if (!prefix.empty() && prefix.back() != '/') {
    prefix += '/';
  }

  minio::s3::ListObjectsArgs list_args;
  list_args.bucket = config_.bucket;
  list_args.prefix = prefix;
  list_args.recursive = false;

  auto result = client_->ListObjects(list_args);
  if (result) {
    minio::s3::Item item = *result;
    if (item) {
      *exists = true;
      return Status::OK();
    }
  }

  *exists = false;
  return Status::OK();
#else
  (void)path;
  (void)exists;
  return Status::NotImplemented("S3 support not compiled");
#endif
}

} // namespace anycache
