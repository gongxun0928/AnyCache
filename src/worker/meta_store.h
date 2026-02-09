#pragma once

#include "common/status.h"
#include "common/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef ANYCACHE_HAS_ROCKSDB
#include <rocksdb/db.h>
#endif

namespace anycache {

// ─── Block metadata persisted in RocksDB ─────────────────────
// Note: file_id and offset_in_file are derivable from the composite BlockId
// via GetInodeId(block_id) and GetBlockIndex(block_id) * block_size.
struct BlockMeta {
  BlockId block_id = kInvalidBlockId;
  uint64_t length = 0; // actual data length
  TierType tier = TierType::kMemory;
  int64_t create_time_ms = 0;
  int64_t last_access_time_ms = 0;
  uint64_t access_count = 0;

  // Serialize/Deserialize to binary
  std::string Serialize() const;
  static BlockMeta Deserialize(const std::string &data);
};

// ─── MetaStore interface ─────────────────────────────────────
// Persists block metadata so that a worker can recover its cache
// index after restart. Backed by RocksDB when available, falls
// back to an in-memory map otherwise.
class MetaStore {
public:
  virtual ~MetaStore() = default;

  virtual Status Open(const std::string &db_path) = 0;
  virtual Status Close() = 0;

  // Block metadata CRUD
  virtual Status PutBlockMeta(BlockId id, const BlockMeta &meta) = 0;
  virtual Status GetBlockMeta(BlockId id, BlockMeta *meta) = 0;
  virtual Status DeleteBlockMeta(BlockId id) = 0;

  // Scan all block metadata (for recovery)
  virtual Status ScanAll(std::vector<BlockMeta> *out) = 0;

  // Factory: creates RocksDB-backed or in-memory store
  static std::unique_ptr<MetaStore> Create(const std::string &db_path);
};

// ─── RocksDB implementation ──────────────────────────────────
#ifdef ANYCACHE_HAS_ROCKSDB
class RocksMetaStore : public MetaStore {
public:
  RocksMetaStore() = default;
  ~RocksMetaStore() override;

  Status Open(const std::string &db_path) override;
  Status Close() override;

  Status PutBlockMeta(BlockId id, const BlockMeta &meta) override;
  Status GetBlockMeta(BlockId id, BlockMeta *meta) override;
  Status DeleteBlockMeta(BlockId id) override;
  Status ScanAll(std::vector<BlockMeta> *out) override;

private:
  std::string MakeKey(BlockId id) const;

  std::unique_ptr<rocksdb::DB> db_;
};
#endif

// ─── In-memory fallback ──────────────────────────────────────
class InMemoryMetaStore : public MetaStore {
public:
  Status Open(const std::string &db_path) override;
  Status Close() override;

  Status PutBlockMeta(BlockId id, const BlockMeta &meta) override;
  Status GetBlockMeta(BlockId id, BlockMeta *meta) override;
  Status DeleteBlockMeta(BlockId id) override;
  Status ScanAll(std::vector<BlockMeta> *out) override;

private:
  std::mutex mu_;
  std::unordered_map<BlockId, BlockMeta> store_;
};

} // namespace anycache
