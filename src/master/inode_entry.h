#pragma once

#include "common/types.h"
#include "master/inode_tree.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace anycache {

// ─── InodeEntry: compact binary format for RocksDB persistence ───
//
// Fields are ordered by decreasing alignment requirement so that the
// struct is naturally aligned without __attribute__((packed)).
//
// Fields NOT stored here (recovered from other sources):
//   - id        : stored as the inodes CF key
//   - children  : reconstructed from edges CF
//
// owner/group use dictionary encoding (uint8_t id) to avoid repeating
// the same strings in every inode entry.  name is stored redundantly
// (also in edges CF key) so that GetInode() returns a complete Inode
// without a reverse edge lookup.
//
struct InodeEntry {
  // ─── 8-byte aligned ───
  uint64_t parent_id;
  uint64_t size;
  uint64_t block_size;
  int64_t creation_time_ms;
  int64_t modification_time_ms;
  // ─── 4-byte aligned ───
  uint32_t mode;
  // ─── 1-byte ───
  uint8_t flags;    // bit0: is_directory, bit1: is_complete
  uint8_t owner_id; // dictionary-encoded owner (0 = empty)
  uint8_t group_id; // dictionary-encoded group (0 = empty)
  uint8_t _padding;
  // Variable part follows: name (remaining bytes)
};

static_assert(sizeof(InodeEntry) == 48, "InodeEntry should be 48 bytes");

// Flag bit positions
constexpr uint8_t kInodeEntryFlagDirectory = 0x01;
constexpr uint8_t kInodeEntryFlagComplete = 0x02;

// ─── Owner/Group dictionary ──────────────────────────────────────
//
// Maps owner/group strings to uint8_t IDs (1~255).  ID 0 means empty
// string.  The dictionary is small (dozens of entries), loaded fully
// into memory, and persisted in RocksDB as special keys.
//
class OwnerGroupDict {
public:
  // Get the ID for an owner string.  If not yet in the dictionary,
  // assign a new ID.  Returns 0 for empty string.
  uint8_t GetOrAddOwnerId(const std::string &owner) {
    return GetOrAdd(owner, owner_to_id_, owners_);
  }
  uint8_t GetOrAddGroupId(const std::string &group) {
    return GetOrAdd(group, group_to_id_, groups_);
  }

  // Lookup string by ID.  Returns "" for ID 0 or unknown ID.
  const std::string &GetOwner(uint8_t id) const { return Lookup(id, owners_); }
  const std::string &GetGroup(uint8_t id) const { return Lookup(id, groups_); }

  // Serialization for RocksDB persistence.
  // Format: [count (1B)] [len (1B) | string] ...
  // Index in the list == ID - 1  (ID 0 is reserved for empty).
  static std::string SerializeList(const std::vector<std::string> &list) {
    std::string buf;
    buf.push_back(static_cast<char>(std::min(list.size(), size_t(255))));
    for (auto &s : list) {
      uint8_t len = static_cast<uint8_t>(std::min(s.size(), size_t(255)));
      buf.push_back(static_cast<char>(len));
      buf.append(s.data(), len);
    }
    return buf;
  }

  static std::vector<std::string> DeserializeList(const std::string &data) {
    std::vector<std::string> list;
    if (data.empty())
      return list;
    uint8_t count = static_cast<uint8_t>(data[0]);
    size_t pos = 1;
    for (uint8_t i = 0; i < count && pos < data.size(); ++i) {
      uint8_t len = static_cast<uint8_t>(data[pos++]);
      size_t actual = std::min(static_cast<size_t>(len), data.size() - pos);
      list.emplace_back(data.data() + pos, actual);
      pos += actual;
    }
    return list;
  }

  std::string SerializeOwners() const { return SerializeList(owners_); }
  std::string SerializeGroups() const { return SerializeList(groups_); }

  void LoadOwners(const std::string &data) {
    owners_ = DeserializeList(data);
    RebuildMap(owners_, owner_to_id_);
  }
  void LoadGroups(const std::string &data) {
    groups_ = DeserializeList(data);
    RebuildMap(groups_, group_to_id_);
  }

  size_t OwnerCount() const { return owners_.size(); }
  size_t GroupCount() const { return groups_.size(); }

  // Check if any new entries were added since last persist
  bool IsDirty() const { return dirty_; }
  void ClearDirty() { dirty_ = false; }

private:
  static const std::string kEmpty;

  uint8_t GetOrAdd(const std::string &s,
                   std::unordered_map<std::string, uint8_t> &map,
                   std::vector<std::string> &list) {
    if (s.empty())
      return 0;
    auto it = map.find(s);
    if (it != map.end())
      return it->second;
    if (list.size() >= 255)
      return 0; // overflow, treat as empty
    list.push_back(s);
    uint8_t id = static_cast<uint8_t>(list.size()); // 1-based
    map[s] = id;
    dirty_ = true;
    return id;
  }

  const std::string &Lookup(uint8_t id,
                            const std::vector<std::string> &list) const {
    if (id == 0 || id > list.size())
      return kEmpty;
    return list[id - 1]; // 1-based → 0-based
  }

  static void RebuildMap(const std::vector<std::string> &list,
                         std::unordered_map<std::string, uint8_t> &map) {
    map.clear();
    for (size_t i = 0; i < list.size(); ++i) {
      map[list[i]] = static_cast<uint8_t>(i + 1); // 1-based
    }
  }

  std::vector<std::string> owners_;
  std::vector<std::string> groups_;
  std::unordered_map<std::string, uint8_t> owner_to_id_;
  std::unordered_map<std::string, uint8_t> group_to_id_;
  bool dirty_ = false;
};

inline const std::string OwnerGroupDict::kEmpty;

// ─── Inode <-> InodeEntry serialization ──────────────────────────

// Serialize an Inode to a binary string for RocksDB value.
// Output: [InodeEntry header (48B)] [name bytes]
// owner/group are dictionary-encoded into the header.
inline std::string SerializeInodeEntry(const Inode &inode,
                                       OwnerGroupDict &dict) {
  InodeEntry hdr{};
  hdr.parent_id = inode.parent_id;
  hdr.size = inode.size;
  hdr.block_size = inode.block_size;
  hdr.creation_time_ms = inode.creation_time_ms;
  hdr.modification_time_ms = inode.modification_time_ms;
  hdr.mode = inode.mode;
  hdr.flags = (inode.is_directory ? kInodeEntryFlagDirectory : 0) |
              (inode.is_complete ? kInodeEntryFlagComplete : 0);
  hdr.owner_id = dict.GetOrAddOwnerId(inode.owner);
  hdr.group_id = dict.GetOrAddGroupId(inode.group);
  hdr._padding = 0;

  std::string buf(sizeof(hdr) + inode.name.size(), '\0');
  std::memcpy(buf.data(), &hdr, sizeof(hdr));
  if (!inode.name.empty()) {
    std::memcpy(buf.data() + sizeof(hdr), inode.name.data(), inode.name.size());
  }
  return buf;
}

// Deserialize an Inode from a binary string.
// `id` comes from the inodes CF key.
// `name` and `owner`/`group` are recovered from the serialized data + dict.
// `children` is NOT restored here — rebuilt from edges CF separately.
inline Inode DeserializeInodeEntry(InodeId id, const std::string &data,
                                   const OwnerGroupDict &dict) {
  Inode inode;
  inode.id = id;

  if (data.size() < sizeof(InodeEntry)) {
    return inode; // malformed data, return defaults
  }

  InodeEntry hdr;
  std::memcpy(&hdr, data.data(), sizeof(hdr));

  inode.parent_id = hdr.parent_id;
  inode.size = hdr.size;
  inode.block_size = hdr.block_size;
  inode.creation_time_ms = hdr.creation_time_ms;
  inode.modification_time_ms = hdr.modification_time_ms;
  inode.mode = hdr.mode;
  inode.is_directory = (hdr.flags & kInodeEntryFlagDirectory) != 0;
  inode.is_complete = (hdr.flags & kInodeEntryFlagComplete) != 0;
  inode.owner = dict.GetOwner(hdr.owner_id);
  inode.group = dict.GetGroup(hdr.group_id);

  // Variable part: name
  if (data.size() > sizeof(hdr)) {
    inode.name.assign(data.data() + sizeof(hdr), data.size() - sizeof(hdr));
  }

  return inode;
}

// ─── Key encoding helpers ────────────────────────────────────────
//
// All keys use big-endian encoding for correct lexicographic ordering
// in RocksDB, consistent with Worker MetaStore's BlockId key format.
//

// Encode a uint64 as 8-byte big-endian string.
inline std::string EncodeBigEndian64(uint64_t val) {
  std::string key(8, '\0');
  for (int i = 7; i >= 0; --i) {
    key[7 - i] = static_cast<char>((val >> (i * 8)) & 0xFF);
  }
  return key;
}

// Decode a uint64 from 8-byte big-endian string.
inline uint64_t DecodeBigEndian64(const char *data) {
  uint64_t val = 0;
  for (int i = 0; i < 8; ++i) {
    val = (val << 8) | static_cast<uint8_t>(data[i]);
  }
  return val;
}

// ─── inodes CF key ───────────────────────────────────────────────

inline std::string EncodeInodeKey(InodeId id) { return EncodeBigEndian64(id); }

inline InodeId DecodeInodeKey(const char *data) {
  return DecodeBigEndian64(data);
}

// Special keys in inodes CF (sorted after all valid InodeIds):
//   0xFFFFFFFFFFFFFFFD → owner dictionary
//   0xFFFFFFFFFFFFFFFE → group dictionary
//   0xFFFFFFFFFFFFFFFF → next_id counter
constexpr uint64_t kOwnerDictKey = 0xFFFFFFFFFFFFFFFD;
constexpr uint64_t kGroupDictKey = 0xFFFFFFFFFFFFFFFE;
constexpr uint64_t kNextIdKey = 0xFFFFFFFFFFFFFFFF;

inline std::string EncodeOwnerDictKey() {
  return EncodeBigEndian64(kOwnerDictKey);
}
inline std::string EncodeGroupDictKey() {
  return EncodeBigEndian64(kGroupDictKey);
}
inline std::string EncodeNextIdKey() { return EncodeBigEndian64(kNextIdKey); }

// next_id value encoding (8 bytes, native endian).
inline std::string EncodeNextIdValue(InodeId next_id) {
  std::string val(8, '\0');
  std::memcpy(val.data(), &next_id, 8);
  return val;
}

inline InodeId DecodeNextIdValue(const char *data) {
  InodeId val;
  std::memcpy(&val, data, 8);
  return val;
}

// ─── edges CF key ────────────────────────────────────────────────

// Encode an edge key: [ParentId (8B big-endian)][ChildName (variable)]
inline std::string EncodeEdgeKey(InodeId parent_id,
                                 const std::string &child_name) {
  std::string key(8 + child_name.size(), '\0');
  std::string parent_enc = EncodeBigEndian64(parent_id);
  std::memcpy(key.data(), parent_enc.data(), 8);
  if (!child_name.empty()) {
    std::memcpy(key.data() + 8, child_name.data(), child_name.size());
  }
  return key;
}

// Decode an edge key into (parent_id, child_name).
inline std::pair<InodeId, std::string> DecodeEdgeKey(const char *data,
                                                     size_t len) {
  InodeId parent_id = 0;
  std::string child_name;
  if (len >= 8) {
    parent_id = DecodeBigEndian64(data);
    if (len > 8) {
      child_name.assign(data + 8, len - 8);
    }
  }
  return {parent_id, child_name};
}

// Encode the edge key prefix for a parent directory (for prefix scan).
inline std::string EncodeEdgePrefix(InodeId parent_id) {
  return EncodeBigEndian64(parent_id);
}

// ─── edges CF value ──────────────────────────────────────────────

inline std::string EncodeEdgeValue(InodeId child_id) {
  return EncodeBigEndian64(child_id);
}

inline InodeId DecodeEdgeValue(const char *data) {
  return DecodeBigEndian64(data);
}

} // namespace anycache
