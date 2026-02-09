#include "fuse/fuse_operations.h"
#include "client/client_config.h"
#include "common/logging.h"
#include "common/metrics.h"

#include <cerrno>
#include <cstring>
#include <ctime>

namespace anycache {

static std::unique_ptr<FuseContext> g_fuse_ctx;

FuseContext *GetFuseContext() { return g_fuse_ctx.get(); }

void InitFuseContext(const Config &cfg) {
  g_fuse_ctx = std::make_unique<FuseContext>();
  g_fuse_ctx->config = cfg.fuse;

  ClientConfig client_config;
  client_config.master_address = cfg.fuse.master_address;
  client_config.master_rpc_timeout_ms = cfg.rpc.master_rpc_timeout_ms;
  client_config.worker_rpc_timeout_ms = cfg.rpc.worker_rpc_timeout_ms;
  g_fuse_ctx->fs_client = std::make_unique<FileSystemClient>(client_config);
}

#ifdef ANYCACHE_HAS_FUSE

namespace fuse_ops {

void *Init(struct fuse_conn_info * /*conn*/, struct fuse_config *cfg) {
  if (cfg) {
    cfg->kernel_cache = 1;
    cfg->auto_cache = 1;
  }
  LOG_INFO("FUSE filesystem initialized");
  return nullptr;
}

void Destroy(void * /*private_data*/) { LOG_INFO("FUSE filesystem destroyed"); }

int Getattr(const char *path, struct stat *stbuf,
            struct fuse_file_info * /*fi*/) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;

  std::memset(stbuf, 0, sizeof(struct stat));

  ClientFileInfo info;
  auto s = ctx->fs_client->GetFileInfo(path, &info);
  if (!s.ok())
    return -ENOENT;

  if (info.is_directory) {
    stbuf->st_mode = S_IFDIR | info.mode;
    stbuf->st_nlink = 2;
  } else {
    stbuf->st_mode = S_IFREG | info.mode;
    stbuf->st_nlink = 1;
    stbuf->st_size = static_cast<off_t>(info.size);
  }
  stbuf->st_uid = getuid();
  stbuf->st_gid = getgid();
  stbuf->st_atime = info.modification_time_ms / 1000;
  stbuf->st_mtime = info.modification_time_ms / 1000;
  stbuf->st_ctime = info.modification_time_ms / 1000;
  return 0;
}

int Readdir(const char *path, void *buf, fuse_fill_dir_t filler,
            off_t /*offset*/, struct fuse_file_info * /*fi*/,
            enum fuse_readdir_flags /*flags*/) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;

  filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
  filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));

  std::vector<ClientFileInfo> children;
  auto s = ctx->fs_client->ListStatus(path, &children);
  if (!s.ok())
    return -ENOENT;

  for (auto &child : children) {
    struct stat st;
    std::memset(&st, 0, sizeof(st));
    if (child.is_directory) {
      st.st_mode = S_IFDIR | child.mode;
    } else {
      st.st_mode = S_IFREG | child.mode;
      st.st_size = static_cast<off_t>(child.size);
    }
    filler(buf, child.name.c_str(), &st, 0,
           static_cast<fuse_fill_dir_flags>(0));
  }
  return 0;
}

int Statfs(const char * /*path*/, struct statvfs *stbuf) {
  std::memset(stbuf, 0, sizeof(struct statvfs));
  stbuf->f_bsize = 4096;
  stbuf->f_frsize = 4096;
  stbuf->f_blocks = 1024 * 1024;
  stbuf->f_bfree = 512 * 1024;
  stbuf->f_bavail = 512 * 1024;
  stbuf->f_namemax = 255;
  return 0;
}

int Open(const char *path, struct fuse_file_info *fi) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;

  ClientFileInfo info;
  auto s = ctx->fs_client->GetFileInfo(path, &info);
  if (!s.ok())
    return -ENOENT;

  uint64_t fh = ctx->next_fh.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(ctx->fh_mu);
    ctx->open_files[fh] = FuseContext::OpenFileState{info.inode_id, info.size};
  }
  fi->fh = fh;
  return 0;
}

int Create(const char *path, mode_t mode, struct fuse_file_info *fi) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;

  auto s = ctx->fs_client->CreateFile(path, mode & 0777);
  if (!s.ok())
    return -EIO;

  // Get the file info to populate the handle
  ClientFileInfo info;
  s = ctx->fs_client->GetFileInfo(path, &info);
  if (!s.ok())
    return -EIO;

  uint64_t fh = ctx->next_fh.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(ctx->fh_mu);
    ctx->open_files[fh] = FuseContext::OpenFileState{info.inode_id, 0};
  }
  fi->fh = fh;
  return 0;
}

int Read(const char *path, char *buf, size_t size, off_t offset,
         struct fuse_file_info * /*fi*/) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;

  size_t bytes_read = 0;
  auto s = ctx->fs_client->ReadFile(path, buf, size, offset, &bytes_read);
  if (!s.ok())
    return -EIO;

  return static_cast<int>(bytes_read);
}

int Write(const char *path, const char *buf, size_t size, off_t offset,
          struct fuse_file_info * /*fi*/) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;

  size_t bytes_written = 0;
  auto s = ctx->fs_client->WriteFile(path, buf, size, offset, &bytes_written);
  if (!s.ok())
    return -EIO;

  return static_cast<int>(bytes_written);
}

int Release(const char * /*path*/, struct fuse_file_info *fi) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;

  std::lock_guard<std::mutex> lock(ctx->fh_mu);
  ctx->open_files.erase(fi->fh);
  return 0;
}

int Truncate(const char *path, off_t size, struct fuse_file_info * /*fi*/) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;
  if (size < 0)
    return -EINVAL;
  auto s = ctx->fs_client->TruncateFile(path, static_cast<uint64_t>(size));
  return s.ok() ? 0 : -EIO;
}

int Unlink(const char *path) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;
  return ctx->fs_client->DeleteFile(path, false).ok() ? 0 : -ENOENT;
}

int Rename(const char *from, const char *to, unsigned int /*flags*/) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;
  return ctx->fs_client->RenameFile(from, to).ok() ? 0 : -ENOENT;
}

int Mkdir(const char *path, mode_t mode) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;
  return ctx->fs_client->Mkdir(path, false).ok() ? 0 : -EIO;
}

int Rmdir(const char *path) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;
  return ctx->fs_client->DeleteFile(path, false).ok() ? 0 : -ENOENT;
}

int Access(const char *path, int /*mask*/) {
  auto *ctx = GetFuseContext();
  if (!ctx)
    return -EIO;
  ClientFileInfo info;
  return ctx->fs_client->GetFileInfo(path, &info).ok() ? 0 : -ENOENT;
}

int Chmod(const char * /*path*/, mode_t /*mode*/,
          struct fuse_file_info * /*fi*/) {
  // AnyCache doesn't persist permissions beyond mode on create; silently
  // succeed
  return 0;
}

int Chown(const char * /*path*/, uid_t /*uid*/, gid_t /*gid*/,
          struct fuse_file_info * /*fi*/) {
  // AnyCache doesn't persist owner/group; silently succeed
  return 0;
}

int Utimens(const char * /*path*/, const struct timespec /*tv*/[2],
            struct fuse_file_info * /*fi*/) {
  // Timestamps are managed by the Master; silently succeed
  return 0;
}

} // namespace fuse_ops

#endif // ANYCACHE_HAS_FUSE
} // namespace anycache
