#include "master/inode_store.h"
#include "common/logging.h"

#include <filesystem>

#include <rocksdb/convenience.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/table.h>

namespace anycache {

InodeStore::~InodeStore() { Close(); }

Status InodeStore::Open(const std::string &db_path) {
  std::filesystem::create_directories(db_path);

  // ─── DB options ───────────────────────────────────────────
  rocksdb::DBOptions db_opts;
  db_opts.create_if_missing = true;
  db_opts.create_missing_column_families = true;
  db_opts.max_open_files = 256;

  // Pick a compression type that is actually linked.
  // LZ4 might not be available in FetchContent builds.
  auto pick_compression = []() -> rocksdb::CompressionType {
    auto supported = rocksdb::GetSupportedCompressions();
    for (auto c : supported) {
      if (c == rocksdb::kLZ4Compression)
        return rocksdb::kLZ4Compression;
    }
    for (auto c : supported) {
      if (c == rocksdb::kSnappyCompression)
        return rocksdb::kSnappyCompression;
    }
    return rocksdb::kNoCompression;
  };
  auto comp = pick_compression();

  // ─── inodes CF: point-query dominant → bloom filter ───────
  rocksdb::ColumnFamilyOptions inodes_cf_opts;
  inodes_cf_opts.compression = comp;
  rocksdb::BlockBasedTableOptions inodes_table_opts;
  inodes_table_opts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
  inodes_cf_opts.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(inodes_table_opts));

  // ─── edges CF: prefix-scan dominant → prefix extractor ────
  rocksdb::ColumnFamilyOptions edges_cf_opts;
  edges_cf_opts.compression = comp;
  edges_cf_opts.prefix_extractor.reset(
      rocksdb::NewFixedPrefixTransform(8)); // ParentId = 8 bytes

  // ─── Default CF (required by RocksDB) ─────────────────────
  rocksdb::ColumnFamilyOptions default_cf_opts;

  std::vector<rocksdb::ColumnFamilyDescriptor> cf_descs = {
      {"default", default_cf_opts},
      {"inodes", inodes_cf_opts},
      {"edges", edges_cf_opts},
  };

  std::vector<rocksdb::ColumnFamilyHandle *> handles;
  rocksdb::DB *raw_db = nullptr;
  auto rdb_status =
      rocksdb::DB::Open(db_opts, db_path, cf_descs, &handles, &raw_db);
  if (!rdb_status.ok()) {
    return Status::IOError("InodeStore RocksDB open failed: " +
                           rdb_status.ToString());
  }
  db_.reset(raw_db);
  // handles[0] = default, handles[1] = inodes, handles[2] = edges
  cf_inodes_ = handles[1];
  cf_edges_ = handles[2];
  // We don't use default CF but must keep the handle alive
  delete handles[0];

  // ─── Load owner/group dictionaries ────────────────────────
  rocksdb::ReadOptions read_opts;
  std::string val;
  if (db_->Get(read_opts, cf_inodes_, EncodeOwnerDictKey(), &val).ok()) {
    dict_.LoadOwners(val);
  }
  val.clear();
  if (db_->Get(read_opts, cf_inodes_, EncodeGroupDictKey(), &val).ok()) {
    dict_.LoadGroups(val);
  }
  dict_.ClearDirty();

  LOG_INFO("InodeStore opened at {}, owners={}, groups={}", db_path,
           dict_.OwnerCount(), dict_.GroupCount());
  return Status::OK();
}

Status InodeStore::Close() {
  if (db_) {
    if (cf_inodes_) {
      db_->DestroyColumnFamilyHandle(cf_inodes_);
      cf_inodes_ = nullptr;
    }
    if (cf_edges_) {
      db_->DestroyColumnFamilyHandle(cf_edges_);
      cf_edges_ = nullptr;
    }
    db_.reset();
  }
  return Status::OK();
}

// ─── Runtime read operations ────────────────────────────────────

Status InodeStore::GetInode(InodeId id, Inode *out) {
  rocksdb::ReadOptions read_opts;
  std::string val;
  auto s = db_->Get(read_opts, cf_inodes_, EncodeInodeKey(id), &val);
  if (s.IsNotFound()) {
    return Status::NotFound("inode not found");
  }
  if (!s.ok()) {
    return Status::IOError("InodeStore GetInode: " + s.ToString());
  }
  *out = DeserializeInodeEntry(id, val, dict_);
  return Status::OK();
}

Status InodeStore::MultiGetInodes(const std::vector<InodeId> &ids,
                                  std::vector<Inode> *out) {
  if (ids.empty()) {
    return Status::OK();
  }

  rocksdb::ReadOptions read_opts;

  // Encode keys
  std::vector<std::string> key_bufs;
  key_bufs.reserve(ids.size());
  std::vector<rocksdb::Slice> keys;
  keys.reserve(ids.size());
  for (auto id : ids) {
    key_bufs.push_back(EncodeInodeKey(id));
    keys.emplace_back(key_bufs.back());
  }

  // Use the MultiGet overload with repeated CF handles
  std::vector<rocksdb::ColumnFamilyHandle *> cfs(ids.size(), cf_inodes_);
  std::vector<std::string> values(ids.size());
  std::vector<rocksdb::Status> statuses =
      db_->MultiGet(read_opts, cfs, keys, &values);

  out->reserve(out->size() + ids.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    if (statuses[i].ok()) {
      out->push_back(DeserializeInodeEntry(ids[i], values[i], dict_));
    }
  }
  return Status::OK();
}

Status InodeStore::GetNextId(InodeId *out) {
  rocksdb::ReadOptions read_opts;
  std::string val;
  auto s = db_->Get(read_opts, cf_inodes_, EncodeNextIdKey(), &val);
  if (s.IsNotFound()) {
    return Status::NotFound("next_id not found");
  }
  if (!s.ok()) {
    return Status::IOError("InodeStore GetNextId: " + s.ToString());
  }
  if (val.size() < 8) {
    return Status::IOError("next_id value truncated");
  }
  *out = DecodeNextIdValue(val.data());
  return Status::OK();
}

// ─── Atomic write operations ────────────────────────────────────

Status InodeStore::CommitBatch(rocksdb::WriteBatch *batch) {
  rocksdb::WriteOptions write_opts;
  write_opts.sync = false; // async for throughput; WAL provides durability
  auto s = db_->Write(write_opts, batch);
  if (!s.ok()) {
    return Status::IOError("InodeStore CommitBatch: " + s.ToString());
  }
  return Status::OK();
}

void InodeStore::BatchPutInode(rocksdb::WriteBatch *batch, InodeId id,
                               const Inode &inode) {
  batch->Put(cf_inodes_, EncodeInodeKey(id), SerializeInodeEntry(inode, dict_));
  MaybePersistDict(batch);
}

void InodeStore::BatchDeleteInode(rocksdb::WriteBatch *batch, InodeId id) {
  batch->Delete(cf_inodes_, EncodeInodeKey(id));
}

void InodeStore::BatchPutEdge(rocksdb::WriteBatch *batch, InodeId parent_id,
                              const std::string &child_name, InodeId child_id) {
  batch->Put(cf_edges_, EncodeEdgeKey(parent_id, child_name),
             EncodeEdgeValue(child_id));
}

void InodeStore::BatchDeleteEdge(rocksdb::WriteBatch *batch, InodeId parent_id,
                                 const std::string &child_name) {
  batch->Delete(cf_edges_, EncodeEdgeKey(parent_id, child_name));
}

void InodeStore::BatchPutNextId(rocksdb::WriteBatch *batch, InodeId next_id) {
  batch->Put(cf_inodes_, EncodeNextIdKey(), EncodeNextIdValue(next_id));
}

void InodeStore::MaybePersistDict(rocksdb::WriteBatch *batch) {
  if (dict_.IsDirty()) {
    batch->Put(cf_inodes_, EncodeOwnerDictKey(), dict_.SerializeOwners());
    batch->Put(cf_inodes_, EncodeGroupDictKey(), dict_.SerializeGroups());
    dict_.ClearDirty();
  }
}

// ─── Recovery operations ────────────────────────────────────────

Status InodeStore::ScanDirectoryInodes(std::vector<Inode> *out) {
  rocksdb::ReadOptions read_opts;
  std::unique_ptr<rocksdb::Iterator> it(
      db_->NewIterator(read_opts, cf_inodes_));

  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    auto key = it->key();
    auto val = it->value();

    // Skip special keys (dict keys and next_id key)
    if (key.size() == 8) {
      uint64_t k = DecodeBigEndian64(key.data());
      if (k >= kOwnerDictKey) {
        continue;
      }
    }

    // Only deserialize directory inodes (check flags byte at offset 44)
    if (val.size() >= sizeof(InodeEntry)) {
      uint8_t flags =
          static_cast<uint8_t>(val.data()[offsetof(InodeEntry, flags)]);
      if (flags & kInodeEntryFlagDirectory) {
        InodeId id = DecodeInodeKey(key.data());
        out->push_back(DeserializeInodeEntry(id, val.ToString(), dict_));
      }
    }
  }

  return it->status().ok() ? Status::OK()
                           : Status::IOError("ScanDirectoryInodes: " +
                                             it->status().ToString());
}

Status InodeStore::ScanAllEdges(
    std::vector<std::tuple<InodeId, std::string, InodeId>> *out) {
  rocksdb::ReadOptions read_opts;
  read_opts.total_order_seek = true; // bypass prefix extractor for full scan
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts, cf_edges_));

  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    auto key = it->key();
    auto val = it->value();
    if (key.size() < 8 || val.size() < 8) {
      continue;
    }

    auto [parent_id, child_name] = DecodeEdgeKey(key.data(), key.size());
    InodeId child_id = DecodeEdgeValue(val.data());
    out->emplace_back(parent_id, std::move(child_name), child_id);
  }

  return it->status().ok()
             ? Status::OK()
             : Status::IOError("ScanAllEdges: " + it->status().ToString());
}

} // namespace anycache
