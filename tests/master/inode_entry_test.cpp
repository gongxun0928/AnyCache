#include "master/inode_entry.h"
#include <gtest/gtest.h>

using namespace anycache;

// ─── OwnerGroupDict ──────────────────────────────────────────────

TEST(OwnerGroupDictTest, EmptyStringReturnsZero) {
  OwnerGroupDict dict;
  EXPECT_EQ(dict.GetOrAddOwnerId(""), 0);
  EXPECT_EQ(dict.GetOrAddGroupId(""), 0);
}

TEST(OwnerGroupDictTest, AssignsSequentialIds) {
  OwnerGroupDict dict;
  EXPECT_EQ(dict.GetOrAddOwnerId("alice"), 1);
  EXPECT_EQ(dict.GetOrAddOwnerId("bob"), 2);
  EXPECT_EQ(dict.GetOrAddOwnerId("charlie"), 3);
  // Duplicate returns existing ID
  EXPECT_EQ(dict.GetOrAddOwnerId("alice"), 1);
  EXPECT_EQ(dict.OwnerCount(), 3u);
}

TEST(OwnerGroupDictTest, LookupById) {
  OwnerGroupDict dict;
  dict.GetOrAddOwnerId("alice");
  dict.GetOrAddGroupId("engineering");
  dict.GetOrAddGroupId("staff");

  EXPECT_EQ(dict.GetOwner(0), "");
  EXPECT_EQ(dict.GetOwner(1), "alice");
  EXPECT_EQ(dict.GetOwner(99), ""); // unknown ID

  EXPECT_EQ(dict.GetGroup(0), "");
  EXPECT_EQ(dict.GetGroup(1), "engineering");
  EXPECT_EQ(dict.GetGroup(2), "staff");
}

TEST(OwnerGroupDictTest, SerializeDeserializeRoundtrip) {
  OwnerGroupDict original;
  original.GetOrAddOwnerId("alice");
  original.GetOrAddOwnerId("bob");
  original.GetOrAddGroupId("engineering");

  std::string owners_data = original.SerializeOwners();
  std::string groups_data = original.SerializeGroups();

  OwnerGroupDict loaded;
  loaded.LoadOwners(owners_data);
  loaded.LoadGroups(groups_data);

  EXPECT_EQ(loaded.OwnerCount(), 2u);
  EXPECT_EQ(loaded.GroupCount(), 1u);
  EXPECT_EQ(loaded.GetOwner(1), "alice");
  EXPECT_EQ(loaded.GetOwner(2), "bob");
  EXPECT_EQ(loaded.GetGroup(1), "engineering");

  // After loading, GetOrAdd should recognize existing entries
  EXPECT_EQ(loaded.GetOrAddOwnerId("alice"), 1);
  EXPECT_EQ(loaded.GetOrAddOwnerId("new_user"), 3);
  EXPECT_EQ(loaded.OwnerCount(), 3u);
}

TEST(OwnerGroupDictTest, DirtyFlag) {
  OwnerGroupDict dict;
  EXPECT_FALSE(dict.IsDirty());

  dict.GetOrAddOwnerId("alice");
  EXPECT_TRUE(dict.IsDirty());

  dict.ClearDirty();
  EXPECT_FALSE(dict.IsDirty());

  // Existing entry does not set dirty
  dict.GetOrAddOwnerId("alice");
  EXPECT_FALSE(dict.IsDirty());

  // New entry sets dirty again
  dict.GetOrAddGroupId("staff");
  EXPECT_TRUE(dict.IsDirty());
}

TEST(OwnerGroupDictTest, SerializeListEmpty) {
  std::string data = OwnerGroupDict::SerializeList({});
  auto list = OwnerGroupDict::DeserializeList(data);
  EXPECT_TRUE(list.empty());
}

TEST(OwnerGroupDictTest, DeserializeEmptyString) {
  auto list = OwnerGroupDict::DeserializeList("");
  EXPECT_TRUE(list.empty());
}

// ─── InodeEntry serialization roundtrip ──────────────────────────

TEST(InodeEntryTest, SerializeDeserializeBasic) {
  OwnerGroupDict dict;

  Inode original;
  original.id = 42;
  original.parent_id = 1;
  original.name = "train.csv";
  original.is_directory = false;
  original.size = 200ULL * 1024 * 1024;
  original.mode = 0644;
  original.owner = "alice";
  original.group = "engineering";
  original.block_size = kDefaultBlockSize;
  original.creation_time_ms = 1700000000000;
  original.modification_time_ms = 1700000001000;
  original.is_complete = true;

  std::string data = SerializeInodeEntry(original, dict);

  // id comes from key; name, owner, group recovered from data + dict
  Inode recovered = DeserializeInodeEntry(42, data, dict);

  EXPECT_EQ(recovered.id, original.id);
  EXPECT_EQ(recovered.parent_id, original.parent_id);
  EXPECT_EQ(recovered.name, original.name);
  EXPECT_EQ(recovered.is_directory, original.is_directory);
  EXPECT_EQ(recovered.size, original.size);
  EXPECT_EQ(recovered.mode, original.mode);
  EXPECT_EQ(recovered.owner, original.owner);
  EXPECT_EQ(recovered.group, original.group);
  EXPECT_EQ(recovered.block_size, original.block_size);
  EXPECT_EQ(recovered.creation_time_ms, original.creation_time_ms);
  EXPECT_EQ(recovered.modification_time_ms, original.modification_time_ms);
  EXPECT_EQ(recovered.is_complete, original.is_complete);
}

TEST(InodeEntryTest, SerializeDeserializeDirectory) {
  OwnerGroupDict dict;

  Inode original;
  original.id = 5;
  original.parent_id = 1;
  original.name = "models";
  original.is_directory = true;
  original.size = 0;
  original.mode = 0755;
  original.is_complete = true;
  original.creation_time_ms = 1700000000000;
  original.modification_time_ms = 1700000000000;

  std::string data = SerializeInodeEntry(original, dict);
  Inode recovered = DeserializeInodeEntry(5, data, dict);

  EXPECT_TRUE(recovered.is_directory);
  EXPECT_EQ(recovered.mode, 0755u);
  EXPECT_EQ(recovered.parent_id, 1u);
  EXPECT_EQ(recovered.name, "models");
}

TEST(InodeEntryTest, SerializeDeserializeEmptyOwnerGroup) {
  OwnerGroupDict dict;

  Inode original;
  original.id = 10;
  original.parent_id = 2;
  original.name = "empty.txt";
  original.is_directory = false;
  original.size = 1024;
  original.mode = 0644;
  original.owner = "";
  original.group = "";
  original.is_complete = false;

  std::string data = SerializeInodeEntry(original, dict);

  // Header (48B) + name "empty.txt" (9B)
  EXPECT_EQ(data.size(), sizeof(InodeEntry) + 9u);

  Inode recovered = DeserializeInodeEntry(10, data, dict);

  EXPECT_EQ(recovered.owner, "");
  EXPECT_EQ(recovered.group, "");
  EXPECT_EQ(recovered.name, "empty.txt");
  EXPECT_FALSE(recovered.is_complete);
}

TEST(InodeEntryTest, SerializeDeserializeEmptyName) {
  OwnerGroupDict dict;

  Inode original;
  original.id = 1;
  original.parent_id = 0;
  original.name = ""; // root inode has empty name
  original.owner = "root";
  original.group = "root";

  std::string data = SerializeInodeEntry(original, dict);

  // Header only, no variable part
  EXPECT_EQ(data.size(), sizeof(InodeEntry));

  Inode recovered = DeserializeInodeEntry(1, data, dict);

  EXPECT_EQ(recovered.name, "");
  EXPECT_EQ(recovered.owner, "root");
  EXPECT_EQ(recovered.group, "root");
}

TEST(InodeEntryTest, DictSharedAcrossSerializations) {
  OwnerGroupDict dict;

  // Two inodes with same owner/group should get same IDs
  Inode inode1;
  inode1.id = 10;
  inode1.name = "a.txt";
  inode1.owner = "alice";
  inode1.group = "eng";

  Inode inode2;
  inode2.id = 11;
  inode2.name = "b.txt";
  inode2.owner = "alice";
  inode2.group = "eng";

  std::string data1 = SerializeInodeEntry(inode1, dict);
  std::string data2 = SerializeInodeEntry(inode2, dict);

  // Dictionary should have exactly 1 owner and 1 group
  EXPECT_EQ(dict.OwnerCount(), 1u);
  EXPECT_EQ(dict.GroupCount(), 1u);

  // Both should decode correctly
  Inode rec1 = DeserializeInodeEntry(10, data1, dict);
  Inode rec2 = DeserializeInodeEntry(11, data2, dict);

  EXPECT_EQ(rec1.owner, "alice");
  EXPECT_EQ(rec2.owner, "alice");
  EXPECT_EQ(rec1.group, "eng");
  EXPECT_EQ(rec2.group, "eng");
}

TEST(InodeEntryTest, SerializedSizeCompact) {
  OwnerGroupDict dict;

  Inode inode;
  inode.name = "";
  inode.owner = "";
  inode.group = "";

  std::string data = SerializeInodeEntry(inode, dict);
  EXPECT_EQ(data.size(), 48u); // header only, no variable part

  inode.name = "train.csv";
  data = SerializeInodeEntry(inode, dict);
  EXPECT_EQ(data.size(), 48u + 9u); // header + "train.csv"
}

TEST(InodeEntryTest, FlagsEncoding) {
  OwnerGroupDict dict;

  // Neither directory nor complete
  Inode inode;
  inode.is_directory = false;
  inode.is_complete = false;
  std::string data = SerializeInodeEntry(inode, dict);
  InodeEntry hdr;
  std::memcpy(&hdr, data.data(), sizeof(hdr));
  EXPECT_EQ(hdr.flags & kInodeEntryFlagDirectory, 0);
  EXPECT_EQ(hdr.flags & kInodeEntryFlagComplete, 0);

  // Directory and complete
  inode.is_directory = true;
  inode.is_complete = true;
  data = SerializeInodeEntry(inode, dict);
  std::memcpy(&hdr, data.data(), sizeof(hdr));
  EXPECT_NE(hdr.flags & kInodeEntryFlagDirectory, 0);
  EXPECT_NE(hdr.flags & kInodeEntryFlagComplete, 0);
}

// ─── BigEndian64 encoding ────────────────────────────────────────

TEST(InodeEntryTest, BigEndian64Roundtrip) {
  std::vector<uint64_t> vals = {
      0, 1, 255, 256, 0xDEADBEEFCAFEBABE, 0xFFFFFFFFFFFFFFFF};
  for (uint64_t v : vals) {
    std::string enc = EncodeBigEndian64(v);
    ASSERT_EQ(enc.size(), 8u);
    uint64_t dec = DecodeBigEndian64(enc.data());
    EXPECT_EQ(dec, v) << "Failed for value " << v;
  }
}

TEST(InodeEntryTest, BigEndian64Ordering) {
  std::string a = EncodeBigEndian64(100);
  std::string b = EncodeBigEndian64(200);
  std::string c = EncodeBigEndian64(300);
  EXPECT_LT(a, b);
  EXPECT_LT(b, c);
}

// ─── Inode key encoding ─────────────────────────────────────────

TEST(InodeEntryTest, InodeKeyRoundtrip) {
  InodeId id = 12345;
  std::string key = EncodeInodeKey(id);
  EXPECT_EQ(key.size(), 8u);
  EXPECT_EQ(DecodeInodeKey(key.data()), id);
}

TEST(InodeEntryTest, SpecialKeysOrdering) {
  std::string inode_key = EncodeInodeKey(kMaxInodeId);
  std::string owner_key = EncodeOwnerDictKey();
  std::string group_key = EncodeGroupDictKey();
  std::string next_key = EncodeNextIdKey();

  // All special keys should sort after normal inode keys
  EXPECT_GT(owner_key, inode_key);
  EXPECT_GT(group_key, inode_key);
  EXPECT_GT(next_key, inode_key);

  // Specific ordering among special keys
  EXPECT_LT(owner_key, group_key);
  EXPECT_LT(group_key, next_key);
}

TEST(InodeEntryTest, NextIdValueRoundtrip) {
  InodeId next_id = 9999;
  std::string val = EncodeNextIdValue(next_id);
  EXPECT_EQ(val.size(), 8u);
  EXPECT_EQ(DecodeNextIdValue(val.data()), next_id);
}

// ─── Edge key encoding ──────────────────────────────────────────

TEST(InodeEntryTest, EdgeKeyRoundtrip) {
  InodeId parent = 42;
  std::string name = "train.csv";
  std::string key = EncodeEdgeKey(parent, name);
  EXPECT_EQ(key.size(), 8 + name.size());

  auto [dec_parent, dec_name] = DecodeEdgeKey(key.data(), key.size());
  EXPECT_EQ(dec_parent, parent);
  EXPECT_EQ(dec_name, name);
}

TEST(InodeEntryTest, EdgeKeyOrdering) {
  // Same parent: sorted by child name lexicographically
  std::string k1 = EncodeEdgeKey(1, "alpha");
  std::string k2 = EncodeEdgeKey(1, "beta");
  std::string k3 = EncodeEdgeKey(1, "gamma");
  EXPECT_LT(k1, k2);
  EXPECT_LT(k2, k3);

  // Different parents: sorted by parent_id first
  std::string k4 = EncodeEdgeKey(1, "zzz");
  std::string k5 = EncodeEdgeKey(2, "aaa");
  EXPECT_LT(k4, k5);
}

TEST(InodeEntryTest, EdgePrefixScan) {
  InodeId parent = 42;
  std::string prefix = EncodeEdgePrefix(parent);
  EXPECT_EQ(prefix.size(), 8u);

  std::string k1 = EncodeEdgeKey(parent, "a.txt");
  std::string k2 = EncodeEdgeKey(parent, "b.txt");
  EXPECT_EQ(k1.substr(0, 8), prefix);
  EXPECT_EQ(k2.substr(0, 8), prefix);

  std::string k3 = EncodeEdgeKey(parent + 1, "a.txt");
  EXPECT_NE(k3.substr(0, 8), prefix);
}

TEST(InodeEntryTest, EdgeValueRoundtrip) {
  InodeId child = 99;
  std::string val = EncodeEdgeValue(child);
  EXPECT_EQ(val.size(), 8u);
  EXPECT_EQ(DecodeEdgeValue(val.data()), child);
}

// ─── Malformed data handling ─────────────────────────────────────

TEST(InodeEntryTest, DeserializeTruncatedData) {
  OwnerGroupDict dict;
  // Data shorter than header — should return default Inode without crashing
  std::string short_data(10, '\0');
  Inode inode = DeserializeInodeEntry(1, short_data, dict);
  EXPECT_EQ(inode.id, 1u);
  EXPECT_EQ(inode.name, "");
  // All other fields should be default/zero
}
