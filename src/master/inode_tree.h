#pragma once

#include "common/status.h"
#include "common/types.h"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace anycache {

// Forward declaration — avoids pulling in RocksDB headers
class InodeStore;

// In-memory inode representing a file or directory
struct Inode {
  InodeId id = kInvalidInodeId;
  InodeId parent_id = kInvalidInodeId;
  std::string name;
  bool is_directory = false;
  uint64_t size = 0;
  uint32_t mode = 0644;
  std::string owner;
  std::string group;
  size_t block_size = kDefaultBlockSize; // per-file block size
  int64_t creation_time_ms = 0;
  int64_t modification_time_ms = 0;

  // Directory: child name -> InodeId
  std::unordered_map<std::string, InodeId> children;

  // Is the file still being written (not yet completed)?
  bool is_complete = true;
};

// InodeTree maintains the file system namespace.
//
// Two operating modes:
//   1. Pure memory (store_ == nullptr): all inodes in dir_inodes_, no
//      persistence.  Used for unit tests and backward compatibility.
//   2. Two-tier (store_ != nullptr): directories in dir_inodes_ (including
//      children maps), files fetched on-demand from RocksDB via store_.
class InodeTree {
public:
  InodeTree();

  // Inject the persistence store.  Must be called before Recover().
  void SetStore(InodeStore *store);

  // Recover from RocksDB: load directory inodes + rebuild children maps.
  // Only meaningful when store_ is set.
  Status Recover();

  // ─── Path operations ─────────────────────────────────────
  Status GetInodeByPath(const std::string &path, Inode *out) const;
  Status GetInodeById(InodeId id, Inode *out) const;

  // Create a file inode; parent directories must exist
  Status CreateFile(const std::string &path, uint32_t mode, InodeId *out_id);

  // Create a directory inode; if recursive, creates parents
  Status CreateDirectory(const std::string &path, uint32_t mode, bool recursive,
                         InodeId *out_id);

  // Mark a file as complete with its final size
  Status CompleteFile(InodeId id, uint64_t size);

  // Delete an inode (and children if recursive)
  Status Delete(const std::string &path, bool recursive);

  // Rename
  Status Rename(const std::string &src, const std::string &dst);

  // List children of a directory
  Status ListDirectory(const std::string &path,
                       std::vector<Inode> *children) const;

  // Update file size
  Status UpdateSize(InodeId id, uint64_t new_size);

  InodeId GetRootId() const { return root_id_; }
  size_t DirCount() const;

private:
  // Split path into components: "/a/b/c" -> ["a", "b", "c"]
  static std::vector<std::string> SplitPath(const std::string &path);

  // Resolve path to inode id (caller must hold lock).
  // All intermediate components must be directories (in dir_inodes_).
  Status ResolvePathLocked(const std::string &path, InodeId *id) const;

  InodeId AllocateId();

  // Collect all descendant inode IDs and edges for recursive deletion.
  // Only traverses directories in dir_inodes_; file children are collected
  // by their edge information (from parent's children map).
  void CollectSubtreeForDeletion(
      InodeId dir_id, std::vector<std::pair<InodeId, std::string>> *edges,
      std::vector<InodeId> *inode_ids, std::vector<InodeId> *dir_ids) const;

  // Remove a directory and its sub-directories from dir_inodes_.
  void RemoveDirSubtreeFromMemory(InodeId id);

  mutable std::shared_mutex mu_;
  std::unordered_map<InodeId, Inode> dir_inodes_;
  InodeId root_id_ = 1;
  std::atomic<InodeId> next_id_{2}; // 1 = root

  // ─── Persistence ──────────────────────────────────────────
  InodeStore *store_ = nullptr;
  static constexpr InodeId kIdAllocBatchSize = 1000;
  InodeId alloc_end_ = 2; // pre-allocation upper bound
};

} // namespace anycache
