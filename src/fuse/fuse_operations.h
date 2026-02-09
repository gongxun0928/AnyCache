#pragma once

// FUSE operations header.
// Only included when building the FUSE executable (ANYCACHE_HAS_FUSE).

#ifdef ANYCACHE_HAS_FUSE
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#endif

#include "client/block_client.h"
#include "client/file_system_client.h"
#include "common/config.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace anycache {

// Holds all global state for the FUSE file system.
// Uses RPC clients to communicate with Master and Workers.
struct FuseContext {
  FuseConfig config;
  std::unique_ptr<FileSystemClient> fs_client; // RPC to Master

  // Open file handles: fh -> (inode_id, worker_address)
  struct OpenFileState {
    InodeId inode_id;
    uint64_t size;
  };
  std::mutex fh_mu;
  std::unordered_map<uint64_t, OpenFileState> open_files;
  std::atomic<uint64_t> next_fh{1};
};

// Get the global FUSE context
FuseContext *GetFuseContext();

// Initialize the global FUSE context
void InitFuseContext(const Config &cfg);

// ─── FUSE operation callbacks ────────────────────────────────
#ifdef ANYCACHE_HAS_FUSE

namespace fuse_ops {

void *Init(struct fuse_conn_info *conn, struct fuse_config *cfg);
void Destroy(void *private_data);

int Getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int Readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
            struct fuse_file_info *fi, enum fuse_readdir_flags flags);
int Statfs(const char *path, struct statvfs *stbuf);

int Open(const char *path, struct fuse_file_info *fi);
int Create(const char *path, mode_t mode, struct fuse_file_info *fi);
int Read(const char *path, char *buf, size_t size, off_t offset,
         struct fuse_file_info *fi);
int Write(const char *path, const char *buf, size_t size, off_t offset,
          struct fuse_file_info *fi);
int Release(const char *path, struct fuse_file_info *fi);
int Truncate(const char *path, off_t size, struct fuse_file_info *fi);
int Unlink(const char *path);
int Rename(const char *from, const char *to, unsigned int flags);

int Mkdir(const char *path, mode_t mode);
int Rmdir(const char *path);

int Access(const char *path, int mask);
int Chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
int Chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi);
int Utimens(const char *path, const struct timespec tv[2],
            struct fuse_file_info *fi);

} // namespace fuse_ops

#endif // ANYCACHE_HAS_FUSE
} // namespace anycache
