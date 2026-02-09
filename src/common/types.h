#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>

namespace anycache {

// ─── Basic type aliases ──────────────────────────────────────
using InodeId = uint64_t;
using BlockId = uint64_t;
using FileId = uint64_t;
using WorkerId = uint64_t;
using UfsFileHandle = int64_t;

constexpr InodeId kInvalidInodeId = 0;
constexpr BlockId kInvalidBlockId = 0;
constexpr FileId kInvalidFileId = 0;
constexpr WorkerId kInvalidWorkerId = 0;
constexpr UfsFileHandle kInvalidUfsHandle = -1;

// ─── Block size constants ────────────────────────────────────
constexpr size_t kDefaultBlockSize = 64 * 1024 * 1024; // 64 MB
constexpr size_t kDefaultPageSize = 1 * 1024 * 1024;   // 1 MB
constexpr size_t kMaxBlockSize = 512 * 1024 * 1024;    // 512 MB

// ─── Composite Block ID ──────────────────────────────────────
// BlockId layout: [InodeId (40 bits) | BlockIndex (24 bits)]
// This encoding makes file-to-block mapping computable (zero storage).
constexpr int kBlockIndexBits = 24;
constexpr uint64_t kBlockIndexMask = (1ULL << kBlockIndexBits) - 1; // 0xFFFFFF
constexpr InodeId kMaxInodeId = (1ULL << 40) - 1;
constexpr uint32_t kMaxBlockIndex = (1U << kBlockIndexBits) - 1;

// Compose a BlockId from InodeId and block index within the file
inline BlockId MakeBlockId(InodeId inode_id, uint32_t block_index) {
  return (static_cast<uint64_t>(inode_id) << kBlockIndexBits) |
         (block_index & kBlockIndexMask);
}

// Extract the InodeId from a composite BlockId
inline InodeId GetInodeId(BlockId block_id) {
  return block_id >> kBlockIndexBits;
}

// Extract the block index from a composite BlockId
inline uint32_t GetBlockIndex(BlockId block_id) {
  return static_cast<uint32_t>(block_id & kBlockIndexMask);
}

// Compute how many blocks a file of `file_size` bytes occupies
inline uint32_t GetBlockCount(uint64_t file_size,
                              size_t block_size = kDefaultBlockSize) {
  if (file_size == 0)
    return 0;
  return static_cast<uint32_t>((file_size + block_size - 1) / block_size);
}

// Compute the actual data length of a given block (last block may be partial)
inline size_t GetBlockLength(uint64_t file_size, uint32_t block_index,
                             size_t block_size = kDefaultBlockSize) {
  uint64_t start = static_cast<uint64_t>(block_index) * block_size;
  if (start >= file_size)
    return 0;
  return static_cast<size_t>(
      std::min(static_cast<uint64_t>(block_size), file_size - start));
}

// ─── Storage tier types ──────────────────────────────────────
enum class TierType : uint8_t {
  kMemory = 0,
  kSSD = 1,
  kHDD = 2,
};

inline const char *TierTypeName(TierType t) {
  switch (t) {
  case TierType::kMemory:
    return "MEM";
  case TierType::kSSD:
    return "SSD";
  case TierType::kHDD:
    return "HDD";
  }
  return "UNKNOWN";
}

// ─── Write policy ────────────────────────────────────────────
enum class WritePolicy : uint8_t {
  kMustCache = 0,    // Only write to worker cache
  kCacheThrough = 1, // Write to cache + UFS synchronously
  kThrough = 2,      // Write to UFS only, no cache
  kAsyncThrough = 3, // Write to cache, async persist to UFS
};

// ─── Read policy ─────────────────────────────────────────────
enum class ReadPolicy : uint8_t {
  kCache = 0,   // Cache data on read
  kNoCache = 1, // Do not cache on read
};

// ─── Block location (shared across master/worker/client) ─────
struct BlockLocationInfo {
  BlockId block_id;
  WorkerId worker_id;
  std::string worker_address;
  TierType tier;
};

// ─── Page key for PageStore ──────────────────────────────────
struct PageKey {
  FileId file_id;
  uint64_t page_index;

  bool operator==(const PageKey &o) const {
    return file_id == o.file_id && page_index == o.page_index;
  }
};

} // namespace anycache

// Hash for PageKey
namespace std {
template <> struct hash<anycache::PageKey> {
  size_t operator()(const anycache::PageKey &k) const {
    size_t h1 = std::hash<uint64_t>{}(k.file_id);
    size_t h2 = std::hash<uint64_t>{}(k.page_index);
    return h1 ^ (h2 << 32 | h2 >> 32);
  }
};
} // namespace std
