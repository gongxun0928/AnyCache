#pragma once

#include "common/status.h"
#include "common/types.h"
#include "master/inode_entry.h"
#include "master/inode_tree.h"

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>

namespace anycache {

// InodeStore persists Master's inode metadata in RocksDB.
//
// Uses two Column Families:
//   - "inodes": InodeId (8B big-endian) → InodeEntry binary
//   - "edges":  ParentId (8B) + ChildName → ChildId (8B)
//
// Also stores owner/group dictionaries and next_id counter
// as special keys in the "inodes" CF.
class InodeStore {
public:
  InodeStore() = default;
  ~InodeStore();

  Status Open(const std::string &db_path);
  Status Close();

  // ─── Runtime read operations ─────────────────────────────

  // Point-query a single Inode (name recovered from InodeEntry).
  Status GetInode(InodeId id, Inode *out);

  // Batch point-query multiple Inodes (for ListDirectory optimization).
  Status MultiGetInodes(const std::vector<InodeId> &ids,
                        std::vector<Inode> *out);

  // Read next_id counter.
  Status GetNextId(InodeId *out);

  // ─── Atomic write operations ─────────────────────────────

  Status CommitBatch(rocksdb::WriteBatch *batch);

  // WriteBatch helper functions
  void BatchPutInode(rocksdb::WriteBatch *batch, InodeId id,
                     const Inode &inode);
  void BatchDeleteInode(rocksdb::WriteBatch *batch, InodeId id);
  void BatchPutEdge(rocksdb::WriteBatch *batch, InodeId parent_id,
                    const std::string &child_name, InodeId child_id);
  void BatchDeleteEdge(rocksdb::WriteBatch *batch, InodeId parent_id,
                       const std::string &child_name);
  void BatchPutNextId(rocksdb::WriteBatch *batch, InodeId next_id);

  // ─── Recovery operations ─────────────────────────────────

  // Scan inodes CF, return only directory Inodes (skip files).
  Status ScanDirectoryInodes(std::vector<Inode> *out);

  // Scan all edges CF entries.
  Status
  ScanAllEdges(std::vector<std::tuple<InodeId, std::string, InodeId>> *out);

private:
  // Persist owner/group dictionaries if dirty.
  void MaybePersistDict(rocksdb::WriteBatch *batch);

  std::unique_ptr<rocksdb::DB> db_;
  rocksdb::ColumnFamilyHandle *cf_inodes_ = nullptr;
  rocksdb::ColumnFamilyHandle *cf_edges_ = nullptr;
  OwnerGroupDict dict_;
};

} // namespace anycache
