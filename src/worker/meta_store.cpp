#include "worker/meta_store.h"
#include "common/logging.h"

#include <chrono>
#include <cstring>
#include <filesystem>

namespace anycache {

// ─── BlockMeta serialization ─────────────────────────────────
// Simple binary format: fixed-size fields concatenated.
std::string BlockMeta::Serialize() const {
  std::string buf(sizeof(BlockMeta), '\0');
  std::memcpy(buf.data(), this, sizeof(BlockMeta));
  return buf;
}

BlockMeta BlockMeta::Deserialize(const std::string &data) {
  BlockMeta meta;
  if (data.size() >= sizeof(BlockMeta)) {
    std::memcpy(&meta, data.data(), sizeof(BlockMeta));
  }
  return meta;
}

// ─── Factory ─────────────────────────────────────────────────
std::unique_ptr<MetaStore> MetaStore::Create(const std::string &db_path) {
#ifdef ANYCACHE_HAS_ROCKSDB
  {
    auto rocks_store = std::make_unique<RocksMetaStore>();
    auto s = rocks_store->Open(db_path);
    if (s.ok()) {
      LOG_INFO("MetaStore: opened RocksDB at {}", db_path);
      return rocks_store;
    }
    LOG_WARN(
        "MetaStore: failed to open RocksDB ({}), falling back to in-memory",
        s.ToString());
  }
#endif
  auto store = std::make_unique<InMemoryMetaStore>();
  store->Open(db_path);
  LOG_INFO("MetaStore: using in-memory store");
  return store;
}

// ═══════════════════════════════════════════════════════════════
// RocksDB implementation
// ═══════════════════════════════════════════════════════════════
#ifdef ANYCACHE_HAS_ROCKSDB

RocksMetaStore::~RocksMetaStore() { Close(); }

Status RocksMetaStore::Open(const std::string &db_path) {
  std::filesystem::create_directories(db_path);

  rocksdb::Options options;
  options.create_if_missing = true;
  options.compression = rocksdb::kLZ4Compression;
  options.max_open_files = 256;
  // Optimise for small values (BlockMeta ~80 bytes)
  options.OptimizeForSmallDb();

  rocksdb::DB *raw_db = nullptr;
  auto rdb_status = rocksdb::DB::Open(options, db_path, &raw_db);
  if (!rdb_status.ok()) {
    return Status::IOError("RocksDB open failed: " + rdb_status.ToString());
  }
  db_.reset(raw_db);
  return Status::OK();
}

Status RocksMetaStore::Close() {
  if (db_) {
    db_.reset();
  }
  return Status::OK();
}

std::string RocksMetaStore::MakeKey(BlockId id) const {
  // Fixed 8-byte big-endian key for ordered iteration
  std::string key(8, '\0');
  for (int i = 7; i >= 0; --i) {
    key[i] = static_cast<char>(id & 0xFF);
    id >>= 8;
  }
  return key;
}

Status RocksMetaStore::PutBlockMeta(BlockId id, const BlockMeta &meta) {
  auto s = db_->Put(rocksdb::WriteOptions(), MakeKey(id), meta.Serialize());
  if (!s.ok())
    return Status::IOError("RocksDB put: " + s.ToString());
  return Status::OK();
}

Status RocksMetaStore::GetBlockMeta(BlockId id, BlockMeta *meta) {
  std::string val;
  auto s = db_->Get(rocksdb::ReadOptions(), MakeKey(id), &val);
  if (s.IsNotFound())
    return Status::NotFound("block not found");
  if (!s.ok())
    return Status::IOError("RocksDB get: " + s.ToString());
  *meta = BlockMeta::Deserialize(val);
  return Status::OK();
}

Status RocksMetaStore::DeleteBlockMeta(BlockId id) {
  auto s = db_->Delete(rocksdb::WriteOptions(), MakeKey(id));
  if (!s.ok())
    return Status::IOError("RocksDB delete: " + s.ToString());
  return Status::OK();
}

Status RocksMetaStore::ScanAll(std::vector<BlockMeta> *out) {
  std::unique_ptr<rocksdb::Iterator> it(
      db_->NewIterator(rocksdb::ReadOptions()));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out->push_back(BlockMeta::Deserialize(it->value().ToString()));
  }
  if (!it->status().ok()) {
    return Status::IOError("RocksDB scan: " + it->status().ToString());
  }
  return Status::OK();
}

#endif // ANYCACHE_HAS_ROCKSDB

// ═══════════════════════════════════════════════════════════════
// In-memory fallback implementation
// ═══════════════════════════════════════════════════════════════

Status InMemoryMetaStore::Open(const std::string & /*db_path*/) {
  return Status::OK();
}

Status InMemoryMetaStore::Close() {
  std::lock_guard<std::mutex> lock(mu_);
  store_.clear();
  return Status::OK();
}

Status InMemoryMetaStore::PutBlockMeta(BlockId id, const BlockMeta &meta) {
  std::lock_guard<std::mutex> lock(mu_);
  store_[id] = meta;
  return Status::OK();
}

Status InMemoryMetaStore::GetBlockMeta(BlockId id, BlockMeta *meta) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = store_.find(id);
  if (it == store_.end())
    return Status::NotFound("block not found");
  *meta = it->second;
  return Status::OK();
}

Status InMemoryMetaStore::DeleteBlockMeta(BlockId id) {
  std::lock_guard<std::mutex> lock(mu_);
  store_.erase(id);
  return Status::OK();
}

Status InMemoryMetaStore::ScanAll(std::vector<BlockMeta> *out) {
  std::lock_guard<std::mutex> lock(mu_);
  out->reserve(store_.size());
  for (auto &[id, meta] : store_) {
    out->push_back(meta);
  }
  return Status::OK();
}

} // namespace anycache
