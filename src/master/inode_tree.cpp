#include "master/inode_tree.h"
#include "common/logging.h"
#include "master/inode_store.h"

#include <chrono>
#include <sstream>

#include <rocksdb/write_batch.h>

namespace anycache {

static int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ─── Construction & recovery ────────────────────────────────────

InodeTree::InodeTree() {
  // Create root inode for pure-memory mode.
  // If SetStore + Recover() is called later, this will be replaced.
  root_id_ = 1;
  Inode root;
  root.id = root_id_;
  root.parent_id = kInvalidInodeId;
  root.name = "";
  root.is_directory = true;
  root.mode = 0755;
  root.creation_time_ms = NowMs();
  root.modification_time_ms = root.creation_time_ms;
  dir_inodes_[root_id_] = std::move(root);
}

void InodeTree::SetStore(InodeStore *store) { store_ = store; }

Status InodeTree::Recover() {
  if (!store_) {
    return Status::OK();
  }

  std::unique_lock lock(mu_);
  dir_inodes_.clear();

  // ① Load directory inodes only (skip all files).
  //    name is recovered from InodeEntry variable part.
  std::vector<Inode> dirs;
  RETURN_IF_ERROR(store_->ScanDirectoryInodes(&dirs));
  for (auto &d : dirs) {
    dir_inodes_[d.id] = std::move(d);
  }

  // ② Load all edges → fill directory children maps.
  //    Edge children may be directories or files; both go into children.
  std::vector<std::tuple<InodeId, std::string, InodeId>> edges;
  RETURN_IF_ERROR(store_->ScanAllEdges(&edges));
  for (auto &[parent_id, name, child_id] : edges) {
    auto it = dir_inodes_.find(parent_id);
    if (it != dir_inodes_.end()) {
      it->second.children[name] = child_id;
    }
  }

  // ③ Recover next_id.
  InodeId stored_next_id = 0;
  if (store_->GetNextId(&stored_next_id).ok() && stored_next_id > 0) {
    next_id_.store(stored_next_id);
    alloc_end_ = stored_next_id;
  } else {
    // Fallback: scan loaded directories for max ID.
    InodeId max_id = 1;
    for (auto &[id, _] : dir_inodes_) {
      max_id = std::max(max_id, id);
    }
    next_id_.store(max_id + 1);
    alloc_end_ = max_id + 1;
  }

  // ④ First-time startup: create and persist root directory.
  if (dir_inodes_.find(root_id_) == dir_inodes_.end()) {
    Inode root{};
    root.id = root_id_;
    root.parent_id = kInvalidInodeId;
    root.name = "";
    root.is_directory = true;
    root.mode = 0755;
    root.creation_time_ms = NowMs();
    root.modification_time_ms = root.creation_time_ms;

    rocksdb::WriteBatch batch;
    store_->BatchPutInode(&batch, root_id_, root);
    RETURN_IF_ERROR(store_->CommitBatch(&batch));

    dir_inodes_[root_id_] = std::move(root);
  }

  LOG_INFO("InodeTree recovered: {} directories loaded", dir_inodes_.size());
  return Status::OK();
}

// ─── Utilities ──────────────────────────────────────────────────

std::vector<std::string> InodeTree::SplitPath(const std::string &path) {
  std::vector<std::string> parts;
  std::istringstream ss(path);
  std::string part;
  while (std::getline(ss, part, '/')) {
    if (!part.empty())
      parts.push_back(part);
  }
  return parts;
}

InodeId InodeTree::AllocateId() {
  InodeId id = next_id_.fetch_add(1);
  if (store_ && id >= alloc_end_) {
    // Current batch exhausted, pre-allocate next batch.
    alloc_end_ = id + kIdAllocBatchSize;
    rocksdb::WriteBatch batch;
    store_->BatchPutNextId(&batch, alloc_end_);
    store_->CommitBatch(&batch); // best-effort
  }
  return id;
}

Status InodeTree::ResolvePathLocked(const std::string &path,
                                    InodeId *id) const {
  auto parts = SplitPath(path);
  InodeId current = root_id_;

  for (const auto &part : parts) {
    auto it = dir_inodes_.find(current);
    if (it == dir_inodes_.end()) {
      return Status::NotFound("inode missing");
    }
    if (!it->second.is_directory) {
      return Status::InvalidArgument("not a directory: " + part);
    }
    auto child_it = it->second.children.find(part);
    if (child_it == it->second.children.end()) {
      return Status::NotFound("path not found: " + path);
    }
    current = child_it->second;
  }
  *id = current;
  return Status::OK();
}

size_t InodeTree::DirCount() const {
  std::shared_lock lock(mu_);
  return dir_inodes_.size();
}

// ─── Read operations ────────────────────────────────────────────

Status InodeTree::GetInodeByPath(const std::string &path, Inode *out) const {
  std::shared_lock lock(mu_);
  InodeId id;
  RETURN_IF_ERROR(ResolvePathLocked(path, &id));

  // Check memory first (directories, or all inodes in pure-memory mode)
  auto it = dir_inodes_.find(id);
  if (it != dir_inodes_.end()) {
    *out = it->second;
    return Status::OK();
  }

  // File: fetch from store
  if (store_) {
    return store_->GetInode(id, out);
  }

  return Status::NotFound("inode missing");
}

Status InodeTree::GetInodeById(InodeId id, Inode *out) const {
  std::shared_lock lock(mu_);

  auto it = dir_inodes_.find(id);
  if (it != dir_inodes_.end()) {
    *out = it->second;
    return Status::OK();
  }

  if (store_) {
    return store_->GetInode(id, out);
  }

  return Status::NotFound("inode not found");
}

Status InodeTree::ListDirectory(const std::string &path,
                                std::vector<Inode> *children) const {
  std::shared_lock lock(mu_);

  InodeId id;
  RETURN_IF_ERROR(ResolvePathLocked(path, &id));

  auto it = dir_inodes_.find(id);
  if (it == dir_inodes_.end()) {
    return Status::NotFound("directory not found");
  }
  if (!it->second.is_directory) {
    return Status::InvalidArgument("not a directory");
  }

  if (!store_) {
    // Pure-memory mode: all inodes in dir_inodes_
    for (auto &[name, child_id] : it->second.children) {
      auto cit = dir_inodes_.find(child_id);
      if (cit != dir_inodes_.end()) {
        children->push_back(cit->second);
      }
    }
    return Status::OK();
  }

  // Two-tier mode: directories from memory, files from store
  std::vector<InodeId> file_ids;
  std::vector<std::string> file_names;

  for (auto &[name, child_id] : it->second.children) {
    auto cit = dir_inodes_.find(child_id);
    if (cit != dir_inodes_.end()) {
      children->push_back(cit->second);
    } else {
      file_ids.push_back(child_id);
      file_names.push_back(name);
    }
  }

  if (!file_ids.empty()) {
    std::vector<Inode> file_inodes;
    RETURN_IF_ERROR(store_->MultiGetInodes(file_ids, &file_inodes));
    for (auto &fi : file_inodes) {
      children->push_back(std::move(fi));
    }
  }

  return Status::OK();
}

// ─── Write operations ───────────────────────────────────────────

Status InodeTree::CreateFile(const std::string &path, uint32_t mode,
                             InodeId *out_id) {
  auto parts = SplitPath(path);
  if (parts.empty()) {
    return Status::InvalidArgument("empty path");
  }

  std::unique_lock lock(mu_);

  // Resolve parent directory
  InodeId parent_id = root_id_;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    auto it = dir_inodes_.find(parent_id);
    if (it == dir_inodes_.end()) {
      return Status::NotFound("parent missing");
    }
    auto child_it = it->second.children.find(parts[i]);
    if (child_it == it->second.children.end()) {
      return Status::NotFound("parent directory not found: " + parts[i]);
    }
    parent_id = child_it->second;
  }

  auto &parent = dir_inodes_[parent_id];
  if (!parent.is_directory) {
    return Status::InvalidArgument("parent is not a directory");
  }

  const std::string &filename = parts.back();
  if (parent.children.count(filename)) {
    return Status::AlreadyExists("file already exists: " + path);
  }

  InodeId new_id = AllocateId();
  Inode inode;
  inode.id = new_id;
  inode.parent_id = parent_id;
  inode.name = filename;
  inode.is_directory = false;
  inode.mode = mode;
  inode.creation_time_ms = NowMs();
  inode.modification_time_ms = inode.creation_time_ms;
  inode.is_complete = false;

  if (store_) {
    // Persist first, then update memory.
    rocksdb::WriteBatch batch;
    store_->BatchPutInode(&batch, new_id, inode);
    store_->BatchPutEdge(&batch, parent_id, filename, new_id);
    RETURN_IF_ERROR(store_->CommitBatch(&batch));

    parent.children[filename] = new_id;
    // File does NOT go into dir_inodes_ (two-tier model).
  } else {
    // Pure-memory mode: all inodes in dir_inodes_.
    parent.children[filename] = new_id;
    dir_inodes_[new_id] = std::move(inode);
  }

  *out_id = new_id;
  return Status::OK();
}

Status InodeTree::CreateDirectory(const std::string &path, uint32_t mode,
                                  bool recursive, InodeId *out_id) {
  auto parts = SplitPath(path);
  if (parts.empty()) {
    *out_id = root_id_;
    return Status::OK();
  }

  std::unique_lock lock(mu_);

  InodeId current = root_id_;
  for (size_t i = 0; i < parts.size(); ++i) {
    auto &node = dir_inodes_[current];
    if (!node.is_directory) {
      return Status::InvalidArgument("not a directory");
    }

    auto child_it = node.children.find(parts[i]);
    if (child_it != node.children.end()) {
      current = child_it->second;
      if (i + 1 == parts.size()) {
        *out_id = current;
        return Status::AlreadyExists("directory exists: " + path);
      }
      continue;
    }

    if (!recursive && i + 1 < parts.size()) {
      return Status::NotFound("parent not found: " + parts[i]);
    }

    InodeId new_id = AllocateId();
    Inode dir;
    dir.id = new_id;
    dir.parent_id = current;
    dir.name = parts[i];
    dir.is_directory = true;
    dir.mode = mode;
    dir.creation_time_ms = NowMs();
    dir.modification_time_ms = dir.creation_time_ms;

    if (store_) {
      rocksdb::WriteBatch batch;
      store_->BatchPutInode(&batch, new_id, dir);
      store_->BatchPutEdge(&batch, current, parts[i], new_id);
      RETURN_IF_ERROR(store_->CommitBatch(&batch));
    }

    dir_inodes_[current].children[parts[i]] = new_id;
    dir_inodes_[new_id] = std::move(dir);
    current = new_id;
  }

  *out_id = current;
  return Status::OK();
}

Status InodeTree::CompleteFile(InodeId id, uint64_t size) {
  std::unique_lock lock(mu_);

  if (store_) {
    // Two-tier mode: read-modify-write via RocksDB.
    Inode inode;
    auto s = store_->GetInode(id, &inode);
    if (!s.ok()) {
      // Fallback: check dir_inodes_ (shouldn't happen for files)
      auto it = dir_inodes_.find(id);
      if (it == dir_inodes_.end()) {
        return Status::NotFound("file not found");
      }
      return Status::InvalidArgument("cannot complete a directory");
    }
    if (inode.is_directory) {
      return Status::InvalidArgument("cannot complete a directory");
    }

    inode.size = size;
    inode.is_complete = true;
    inode.modification_time_ms = NowMs();

    rocksdb::WriteBatch batch;
    store_->BatchPutInode(&batch, id, inode);
    return store_->CommitBatch(&batch);
  }

  // Pure-memory mode
  auto it = dir_inodes_.find(id);
  if (it == dir_inodes_.end()) {
    return Status::NotFound("file not found");
  }
  if (it->second.is_directory) {
    return Status::InvalidArgument("cannot complete a directory");
  }
  it->second.size = size;
  it->second.is_complete = true;
  it->second.modification_time_ms = NowMs();
  return Status::OK();
}

Status InodeTree::Delete(const std::string &path, bool recursive) {
  auto parts = SplitPath(path);
  if (parts.empty()) {
    return Status::InvalidArgument("cannot delete root");
  }

  std::unique_lock lock(mu_);

  InodeId id;
  auto s = ResolvePathLocked(path, &id);
  if (!s.ok()) {
    return s;
  }

  // Find target info: is it a directory? what's its parent?
  bool is_dir = (dir_inodes_.count(id) > 0);
  InodeId parent_id = kInvalidInodeId;
  std::string target_name = parts.back();

  if (is_dir) {
    auto &inode = dir_inodes_[id];
    parent_id = inode.parent_id;
    if (!inode.children.empty() && !recursive) {
      return Status::InvalidArgument("directory not empty");
    }
  } else if (store_) {
    // It's a file in RocksDB. Get parent_id from the path.
    // The parent is the directory that contains this file.
    InodeId pid = root_id_;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      auto pit = dir_inodes_.find(pid);
      if (pit == dir_inodes_.end()) {
        return Status::NotFound("parent missing");
      }
      auto cit = pit->second.children.find(parts[i]);
      if (cit == pit->second.children.end()) {
        return Status::NotFound("parent missing");
      }
      pid = cit->second;
    }
    parent_id = pid;
  } else {
    // Pure-memory mode: shouldn't reach here since all inodes are
    // in dir_inodes_, but handle gracefully.
    return Status::NotFound("inode not found");
  }

  if (store_) {
    rocksdb::WriteBatch batch;

    // Delete the target itself
    store_->BatchDeleteInode(&batch, id);
    store_->BatchDeleteEdge(&batch, parent_id, target_name);

    // If directory and recursive, collect entire subtree
    if (is_dir && recursive) {
      std::vector<std::pair<InodeId, std::string>> sub_edges;
      std::vector<InodeId> sub_inodes;
      std::vector<InodeId> sub_dirs;
      CollectSubtreeForDeletion(id, &sub_edges, &sub_inodes, &sub_dirs);

      for (auto &[pid, cname] : sub_edges) {
        store_->BatchDeleteEdge(&batch, pid, cname);
      }
      for (auto iid : sub_inodes) {
        store_->BatchDeleteInode(&batch, iid);
      }
    }

    RETURN_IF_ERROR(store_->CommitBatch(&batch));

    // Update memory
    auto pit = dir_inodes_.find(parent_id);
    if (pit != dir_inodes_.end()) {
      pit->second.children.erase(target_name);
    }
    if (is_dir) {
      if (recursive) {
        RemoveDirSubtreeFromMemory(id);
      }
      dir_inodes_.erase(id);
    }
  } else {
    // Pure-memory mode: original logic
    auto &parent = dir_inodes_[dir_inodes_[id].parent_id];
    parent.children.erase(dir_inodes_[id].name);

    std::vector<InodeId> to_remove = {id};
    while (!to_remove.empty()) {
      InodeId rid = to_remove.back();
      to_remove.pop_back();
      auto rit = dir_inodes_.find(rid);
      if (rit != dir_inodes_.end()) {
        for (auto &[_, child_id] : rit->second.children) {
          to_remove.push_back(child_id);
        }
        dir_inodes_.erase(rit);
      }
    }
  }

  return Status::OK();
}

Status InodeTree::Rename(const std::string &src, const std::string &dst) {
  auto src_parts = SplitPath(src);
  auto dst_parts = SplitPath(dst);
  if (src_parts.empty() || dst_parts.empty()) {
    return Status::InvalidArgument("invalid path");
  }

  std::unique_lock lock(mu_);

  InodeId src_id;
  RETURN_IF_ERROR(ResolvePathLocked(src, &src_id));

  // Resolve destination parent
  InodeId dst_parent_id = root_id_;
  for (size_t i = 0; i + 1 < dst_parts.size(); ++i) {
    auto it = dir_inodes_.find(dst_parent_id);
    if (it == dir_inodes_.end()) {
      return Status::NotFound("dest parent missing");
    }
    auto cit = it->second.children.find(dst_parts[i]);
    if (cit == it->second.children.end()) {
      return Status::NotFound("dest parent not found");
    }
    dst_parent_id = cit->second;
  }

  const std::string &new_name = dst_parts.back();
  auto &new_parent = dir_inodes_[dst_parent_id];
  if (!new_parent.is_directory) {
    return Status::InvalidArgument("destination parent is not a directory");
  }
  if (new_parent.children.count(new_name)) {
    return Status::AlreadyExists("destination exists");
  }

  bool is_dir = (dir_inodes_.count(src_id) > 0);
  InodeId old_parent_id = kInvalidInodeId;
  std::string old_name;

  if (is_dir) {
    old_parent_id = dir_inodes_[src_id].parent_id;
    old_name = dir_inodes_[src_id].name;
  } else if (store_) {
    // File: resolve old parent from path
    InodeId pid = root_id_;
    for (size_t i = 0; i + 1 < src_parts.size(); ++i) {
      auto it = dir_inodes_.find(pid);
      if (it == dir_inodes_.end()) {
        return Status::NotFound("src parent missing");
      }
      auto cit = it->second.children.find(src_parts[i]);
      if (cit == it->second.children.end()) {
        return Status::NotFound("src parent missing");
      }
      pid = cit->second;
    }
    old_parent_id = pid;
    old_name = src_parts.back();
  } else {
    return Status::NotFound("inode not found");
  }

  if (store_) {
    // Read current inode, update parent_id and name, write back
    Inode inode;
    if (is_dir) {
      inode = dir_inodes_[src_id];
    } else {
      RETURN_IF_ERROR(store_->GetInode(src_id, &inode));
    }
    inode.parent_id = dst_parent_id;
    inode.name = new_name;

    rocksdb::WriteBatch batch;
    store_->BatchPutInode(&batch, src_id, inode);
    store_->BatchDeleteEdge(&batch, old_parent_id, old_name);
    store_->BatchPutEdge(&batch, dst_parent_id, new_name, src_id);
    RETURN_IF_ERROR(store_->CommitBatch(&batch));

    // Update memory
    dir_inodes_[old_parent_id].children.erase(old_name);
    dir_inodes_[dst_parent_id].children[new_name] = src_id;
    if (is_dir) {
      dir_inodes_[src_id].name = new_name;
      dir_inodes_[src_id].parent_id = dst_parent_id;
    }
  } else {
    // Pure-memory mode: original logic
    auto &src_inode = dir_inodes_[src_id];
    auto &old_parent = dir_inodes_[src_inode.parent_id];
    old_parent.children.erase(src_inode.name);
    src_inode.name = new_name;
    src_inode.parent_id = dst_parent_id;
    new_parent.children[new_name] = src_id;
  }

  return Status::OK();
}

Status InodeTree::UpdateSize(InodeId id, uint64_t new_size) {
  std::unique_lock lock(mu_);

  if (store_) {
    // Two-tier: read-modify-write via RocksDB
    Inode inode;
    // Try dir_inodes_ first (might be a directory, though unusual)
    auto it = dir_inodes_.find(id);
    if (it != dir_inodes_.end()) {
      it->second.size = new_size;
      it->second.modification_time_ms = NowMs();
      rocksdb::WriteBatch batch;
      store_->BatchPutInode(&batch, id, it->second);
      return store_->CommitBatch(&batch);
    }
    // It's a file
    RETURN_IF_ERROR(store_->GetInode(id, &inode));
    inode.size = new_size;
    inode.modification_time_ms = NowMs();
    rocksdb::WriteBatch batch;
    store_->BatchPutInode(&batch, id, inode);
    return store_->CommitBatch(&batch);
  }

  // Pure-memory mode
  auto it = dir_inodes_.find(id);
  if (it == dir_inodes_.end()) {
    return Status::NotFound("inode not found");
  }
  it->second.size = new_size;
  it->second.modification_time_ms = NowMs();
  return Status::OK();
}

// ─── Private helpers ────────────────────────────────────────────

void InodeTree::CollectSubtreeForDeletion(
    InodeId dir_id, std::vector<std::pair<InodeId, std::string>> *edges,
    std::vector<InodeId> *inode_ids, std::vector<InodeId> *dir_ids) const {
  auto it = dir_inodes_.find(dir_id);
  if (it == dir_inodes_.end()) {
    return;
  }

  for (auto &[name, child_id] : it->second.children) {
    edges->emplace_back(dir_id, name);
    inode_ids->push_back(child_id);

    if (dir_inodes_.count(child_id)) {
      dir_ids->push_back(child_id);
      CollectSubtreeForDeletion(child_id, edges, inode_ids, dir_ids);
    }
  }
}

void InodeTree::RemoveDirSubtreeFromMemory(InodeId id) {
  auto it = dir_inodes_.find(id);
  if (it == dir_inodes_.end()) {
    return;
  }

  for (auto &[_, child_id] : it->second.children) {
    RemoveDirSubtreeFromMemory(child_id);
  }
  // Don't erase 'id' here — the caller handles the root of deletion.
  // Only erase children that are directories.
  for (auto &[_, child_id] : it->second.children) {
    dir_inodes_.erase(child_id);
  }
}

} // namespace anycache
