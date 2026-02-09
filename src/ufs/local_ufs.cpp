#include "ufs/local_ufs.h"
#include "common/logging.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// Convert file_time_type to epoch millisecond timestamp
// Linux (libstdc++): file_clock is system_clock, directly get time_since_epoch
// macOS (libc++):    file_clock is independent, convert via duration delta
inline int64_t FileTimeToMs(fs::file_time_type ftime) {
#if defined(__APPLE__) ||                                                      \
    (defined(_LIBCPP_VERSION) && !defined(__cpp_lib_chrono))
  // libc++'s file_clock lacks to_sys/clock_cast, manually convert using current
  // time as anchor
  auto file_now = fs::file_time_type::clock::now();
  auto sys_now = std::chrono::system_clock::now();
  auto delta = ftime - file_now; // Duration relative to now
  auto sys_time =
      sys_now +
      std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             sys_time.time_since_epoch())
      .count();
#else
  // libstdc++ (GCC/Linux): file_clock supports clock_cast or is directly
  // system_clock
  auto sys_time = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             sys_time.time_since_epoch())
      .count();
#endif
}

} // anonymous namespace

namespace anycache {

LocalUnderFileSystem::LocalUnderFileSystem(const std::string &root_path)
    : root_path_(root_path) {
  // Ensure root path ends without trailing slash (unless root)
  if (!root_path_.empty() && root_path_.back() == '/' &&
      root_path_.size() > 1) {
    root_path_.pop_back();
  }
}

LocalUnderFileSystem::~LocalUnderFileSystem() {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &[handle, fd] : fd_map_) {
    ::close(fd);
  }
  fd_map_.clear();
}

std::string LocalUnderFileSystem::ResolvePath(const std::string &path) const {
  if (root_path_.empty())
    return path;
  if (path.empty() || path == "/")
    return root_path_;
  if (path[0] == '/')
    return root_path_ + path;
  return root_path_ + "/" + path;
}

Status LocalUnderFileSystem::Open(const std::string &path, int flags,
                                  UfsFileHandle *handle) {
  std::string full = ResolvePath(path);
  int fd = ::open(full.c_str(), flags);
  if (fd < 0) {
    return Status::IOError("open failed: " + full + ": " + strerror(errno));
  }

  std::lock_guard<std::mutex> lock(mu_);
  *handle = next_handle_++;
  fd_map_[*handle] = fd;
  return Status::OK();
}

Status LocalUnderFileSystem::Create(const std::string &path,
                                    const CreateOptions &opts,
                                    UfsFileHandle *handle) {
  std::string full = ResolvePath(path);

  if (opts.recursive) {
    fs::path parent = fs::path(full).parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
      return Status::IOError("create_directories failed: " + ec.message());
    }
  }

  int fd = ::open(full.c_str(), O_CREAT | O_WRONLY | O_TRUNC, opts.mode);
  if (fd < 0) {
    return Status::IOError("create failed: " + full + ": " + strerror(errno));
  }

  std::lock_guard<std::mutex> lock(mu_);
  *handle = next_handle_++;
  fd_map_[*handle] = fd;
  return Status::OK();
}

Status LocalUnderFileSystem::Read(UfsFileHandle handle, void *buf, size_t size,
                                  off_t offset, size_t *bytes_read) {
  int fd;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = fd_map_.find(handle);
    if (it == fd_map_.end()) {
      return Status::InvalidArgument("invalid handle");
    }
    fd = it->second;
  }

  ssize_t n = ::pread(fd, buf, size, offset);
  if (n < 0) {
    return Status::IOError("pread failed: " + std::string(strerror(errno)));
  }
  *bytes_read = static_cast<size_t>(n);
  return Status::OK();
}

Status LocalUnderFileSystem::Write(UfsFileHandle handle, const void *buf,
                                   size_t size, off_t offset,
                                   size_t *bytes_written) {
  int fd;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = fd_map_.find(handle);
    if (it == fd_map_.end()) {
      return Status::InvalidArgument("invalid handle");
    }
    fd = it->second;
  }

  ssize_t n = ::pwrite(fd, buf, size, offset);
  if (n < 0) {
    return Status::IOError("pwrite failed: " + std::string(strerror(errno)));
  }
  *bytes_written = static_cast<size_t>(n);
  return Status::OK();
}

Status LocalUnderFileSystem::Close(UfsFileHandle handle) {
  int fd;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = fd_map_.find(handle);
    if (it == fd_map_.end()) {
      return Status::InvalidArgument("invalid handle");
    }
    fd = it->second;
    fd_map_.erase(it);
  }
  if (::close(fd) < 0) {
    return Status::IOError("close failed: " + std::string(strerror(errno)));
  }
  return Status::OK();
}

Status LocalUnderFileSystem::Delete(const std::string &path, bool recursive) {
  std::string full = ResolvePath(path);
  std::error_code ec;

  if (recursive) {
    fs::remove_all(full, ec);
  } else {
    fs::remove(full, ec);
  }
  if (ec) {
    return Status::IOError("delete failed: " + full + ": " + ec.message());
  }
  return Status::OK();
}

Status LocalUnderFileSystem::Rename(const std::string &src,
                                    const std::string &dst) {
  std::string full_src = ResolvePath(src);
  std::string full_dst = ResolvePath(dst);
  std::error_code ec;
  fs::rename(full_src, full_dst, ec);
  if (ec) {
    return Status::IOError("rename failed: " + ec.message());
  }
  return Status::OK();
}

Status LocalUnderFileSystem::ListDir(const std::string &path,
                                     std::vector<UfsFileInfo> *entries) {
  std::string full = ResolvePath(path);
  std::error_code ec;

  for (const auto &entry : fs::directory_iterator(full, ec)) {
    UfsFileInfo info;
    info.name = entry.path().filename().string();
    info.path = entry.path().string();
    info.is_directory = entry.is_directory();
    if (!info.is_directory) {
      info.size = entry.file_size();
    }
    info.modification_time_ms = FileTimeToMs(entry.last_write_time());
    entries->push_back(std::move(info));
  }
  if (ec) {
    return Status::IOError("listdir failed: " + full + ": " + ec.message());
  }
  return Status::OK();
}

Status LocalUnderFileSystem::GetFileInfo(const std::string &path,
                                         UfsFileInfo *info) {
  std::string full = ResolvePath(path);
  struct stat st;
  if (::stat(full.c_str(), &st) < 0) {
    if (errno == ENOENT) {
      return Status::NotFound(full);
    }
    return Status::IOError("stat failed: " + full + ": " + strerror(errno));
  }

  info->name = fs::path(full).filename().string();
  info->path = full;
  info->is_directory = S_ISDIR(st.st_mode);
  info->size = static_cast<uint64_t>(st.st_size);
  info->mode = st.st_mode & 0777;
  info->modification_time_ms = static_cast<int64_t>(st.st_mtime) * 1000;
  return Status::OK();
}

Status LocalUnderFileSystem::Mkdir(const std::string &path,
                                   const MkdirOptions &opts) {
  std::string full = ResolvePath(path);
  std::error_code ec;

  if (opts.recursive) {
    fs::create_directories(full, ec);
  } else {
    fs::create_directory(full, ec);
  }
  if (ec) {
    return Status::IOError("mkdir failed: " + full + ": " + ec.message());
  }
  return Status::OK();
}

Status LocalUnderFileSystem::Exists(const std::string &path, bool *exists) {
  std::string full = ResolvePath(path);
  *exists = fs::exists(full);
  return Status::OK();
}

} // namespace anycache
