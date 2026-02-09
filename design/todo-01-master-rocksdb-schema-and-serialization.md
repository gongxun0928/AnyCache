# TODO-01: Master RocksDB 存储 Schema 与序列化

> 状态: 已完成
> 优先级: P0

## 1. 概述

Master 元数据持久化到 RocksDB，采用两个 Column Family 分离存储 inode 属性和目录树边。序列化采用紧凑二进制格式（memcpy + 变长尾部），与 Worker 的 BlockMeta 风格一致。

RPC 传输仍走 Protobuf `FileInfo`，已补充 `block_size` 和 `is_complete` 字段。

## 2. Column Family 设计

### 2.1 总览

| Column Family | Key | Value | 用途 |
|---|---|---|---|
| `inodes` | `InodeId` (8B 大端) | `InodeEntry` 二进制序列化 | 存储 inode 自身属性 |
| `edges` | `ParentId (8B 大端) + ChildName (变长)` | `ChildId` (8B 大端) | 路径查找 + 目录列表 |

特殊 Key（排在所有正常 InodeId 之后）：

| Column Family | Key | Value | 用途 |
|---|---|---|---|
| `inodes` | `0xFFFFFFFFFFFFFFFD` (8B 大端) | owner 字典序列化 | owner 字符串 → ID 映射 |
| `inodes` | `0xFFFFFFFFFFFFFFFE` (8B 大端) | group 字典序列化 | group 字符串 → ID 映射 |
| `inodes` | `0xFFFFFFFFFFFFFFFF` (8B 大端) | `next_id` (8B 小端) | InodeId 分配计数器 |

### 2.2 edges CF — 路径查找与目录列表

**Key 编码：**

```
[ParentId: 8 bytes, big-endian][ChildName: variable length, raw bytes]
```

- 同一目录的子条目在 RocksDB 中**物理相邻**（ParentId 前缀相同）
- ParentId 固定 8 字节，后续字节即为 ChildName，无需分隔符
- 前缀扫描：`Seek(EncodeEdgePrefix(parent_id))` → 逐条检查前 8 字节是否仍为 parent_id

**Value 编码：**

```
[ChildId: 8 bytes, big-endian]
```

**edges 提供两种查询能力：**

| 查询 | 方式 |
|---|---|
| 路径查找 `(parent_id, "name") → child_id` | 点查 `Get(EncodeEdgeKey(parent_id, name))` |
| 目录列表 `parent_id → [(name, child_id), ...]` | 前缀扫描 `Seek(EncodeEdgePrefix(parent_id))` |

路径解析示例：

```
GetInodeByPath("/data/train.csv")

步骤 1: edges.Get(root_id, "data")     → inode_id = 2
步骤 2: edges.Get(2, "train.csv")      → inode_id = 3
步骤 3: inodes.Get(3)                  → InodeEntry { size, mode, ... }
```

### 2.3 inodes CF — 属性存储

**Key 编码：** 8 字节大端 InodeId（与 Worker MetaStore 的 BlockId key 编码一致）

**Value 编码：** 见第 3 节 InodeEntry 格式

### 2.4 各操作的 WriteBatch 组成

| 操作 | WriteBatch 内容 |
|---|---|
| CreateFile | `Put(inodes, new_id, entry)` + `Put(edges, parent+name, new_id)` + `Put(inodes, NEXT_ID_KEY, next_id)` |
| CreateDirectory | 同 CreateFile，递归时多组 |
| CompleteFile | `Put(inodes, id, updated_entry)` |
| UpdateSize | `Put(inodes, id, updated_entry)` |
| Delete(单个) | `Delete(inodes, id)` + `Delete(edges, parent+name)` |
| Delete(递归) | 子树每个节点：`Delete(inodes, id)` + `Delete(edges, parent+name)` |
| Rename | `Put(inodes, id, updated_entry)` + `Delete(edges, old_parent+old_name)` + `Put(edges, new_parent+new_name, id)` |

### 2.5 RocksDB 配置

```cpp
// inodes CF: 点查为主，加 bloom filter
rocksdb::BlockBasedTableOptions inodes_table_opts;
inodes_table_opts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));

// edges CF: 前缀扫描为主，加 prefix extractor
rocksdb::ColumnFamilyOptions edges_cf_opts;
edges_cf_opts.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(8));

// 通用配置
options.create_if_missing = true;
options.create_missing_column_families = true;
options.compression = rocksdb::kLZ4Compression;
options.max_open_files = 256;
```

### 2.6 next_id 批量预分配

每次预分配 1000 个 ID，仅在跨批时持久化上界：

```
启动时: 读取 next_id = N，持久化 N + 1000
运行时: AllocateId() 返回 N, N+1, ..., N+999 无需写 RocksDB
        达到 N+1000 时再预分配下一批
重启后: 从持久化的上界恢复，最多浪费 999 个 ID（40 位空间下可忽略）
```

## 3. InodeEntry — 持久化格式

### 3.1 不存入 value 的字段

| Inode 字段 | 不存储原因 | 恢复来源 |
|---|---|---|
| `id` | 已在 inodes CF key 中 | key |
| `children` | 由 edges CF 重建 | edges CF 前缀扫描 |

### 3.2 冗余存储的字段

| 字段 | 主存储 | 冗余存储位置 | 理由 |
|---|---|---|---|
| `name` | edges CF key | InodeEntry 变长部分 | GetInodeById 时无需反查 edges CF |

### 3.3 字典编码（owner/group）

owner 和 group 字段在缓存系统中存在大量重复（同一用户/用户组拥有的文件很多），采用枚举值 + 字典编码方式优化：

- **字典**：`OwnerGroupDict` 类维护 owner/group 字符串 → `uint8_t` ID 的双向映射
- **ID 0** = 空字符串，**ID 1~255** = 按插入顺序分配
- **容量**：最多 255 个不同 owner 和 255 个不同 group，对缓存系统足够
- **持久化**：字典以 `[count(1B)][len(1B)|string]...` 格式存入 inodes CF 的特殊 Key
- **加载**：InodeStore 启动时从 RocksDB 加载字典到内存，序列化/反序列化时查表

### 3.4 二进制编码格式

```
┌───────────────────── 固定头部 (48 bytes, 自然对齐) ──────────────────┐
│ parent_id (8B) │ size (8B) │ block_size (8B) │ creation_time_ms (8B)│
│ modification_time_ms (8B) │ mode (4B) │ flags (1B) │ owner_id (1B)  │
│ group_id (1B) │ [1B padding]                                        │
├───────────────────── 变长部分 ──────────────────────────────────────┤
│ name (剩余 bytes)                                                    │
└────────────────────────────────────────────────────────────────────┘

flags: bit 0 = is_directory, bit 1 = is_complete
owner_id / group_id: 字典编码的 uint8_t（0=空，1~255=字典条目）
```

空 name 时总大小 48 字节。字段按大小降序排列，所有字段自然对齐，无需 `__attribute__((packed))`。

相比旧版（变长 owner+group）：
- 每条 InodeEntry 节省 ~10~20 字节（典型 owner+group 长度）
- 额外存储 name（平均 ~15 字节），总大小基本持平
- 但 GetInodeById 不再需要反查 edges CF，读路径更简洁

### 3.5 实现

`InodeEntry` 结构体、`OwnerGroupDict` 类、序列化/反序列化函数、Key 编解码函数均在 `src/master/inode_entry.h` 中实现：

| 类/函数 | 用途 |
|---|---|
| `OwnerGroupDict` | owner/group 字典编码管理（GetOrAdd / Lookup / Serialize / Load） |
| `SerializeInodeEntry(Inode, OwnerGroupDict&) → string` | Inode → RocksDB value（owner/group 查字典编码） |
| `DeserializeInodeEntry(id, data, OwnerGroupDict&) → Inode` | RocksDB value → Inode（id 从 key、name 从变长部分、owner/group 查字典解码） |
| `EncodeBigEndian64(val) → string` | 8 字节大端编码 |
| `DecodeBigEndian64(data) → uint64` | 8 字节大端解码 |
| `EncodeInodeKey(id) → string` | inodes CF key |
| `EncodeEdgeKey(parent_id, name) → string` | edges CF key |
| `DecodeEdgeKey(data, len) → (parent_id, name)` | edges CF key 解码 |
| `EncodeEdgePrefix(parent_id) → string` | edges CF 前缀扫描 key |
| `EncodeEdgeValue(child_id) → string` | edges CF value |
| `DecodeEdgeValue(data) → InodeId` | edges CF value 解码 |
| `EncodeOwnerDictKey() → string` | owner 字典持久化 key |
| `EncodeGroupDictKey() → string` | group 字典持久化 key |
| `EncodeNextIdKey() → string` | next_id 特殊 key |
| `EncodeNextIdValue(id) → string` | next_id value 编码 |
| `DecodeNextIdValue(data) → InodeId` | next_id value 解码 |

## 4. FileInfo Proto 扩展

RPC 传输的 `FileInfo` 已补充两个字段（`proto/common.proto`）：

```protobuf
message FileInfo {
    // ... 原有字段 1~12 ...
    uint64 block_size = 13;     // 文件级别块大小
    bool is_complete = 14;      // 写入是否完成
}
```

`proto_utils.h` 中的 `InodeToProto()` / `ProtoToInode()` 已同步更新。

## 5. 数据结构职责总结

```
┌──────────────────────────────────────────────────────┐
│                   运行时（内存）                       │
│                                                      │
│   Inode {                                            │
│     id, parent_id, name      ← 完整信息              │
│     children                 ← 路径解析快路径          │
│     size, mode, owner ...    ← 属性                  │
│   }                                                  │
└────────────────────┬─────────────────────────────────┘
                     │ 写穿 / 恢复
┌────────────────────▼─────────────────────────────────┐
│                   持久化（RocksDB）                    │
│                                                      │
│   inodes CF:                                         │
│     key = InodeId (8B 大端)                           │
│     val = InodeEntry 二进制  ← 无 id/children          │
│       owner/group 字典编码, name 冗余存储              │
│                                                      │
│   edges CF:                                          │
│     key = ParentId (8B 大端) + ChildName              │
│     val = ChildId (8B 大端)                           │
│                                                      │
│   恢复时: inodes + edges → 重建完整 Inode + children  │
└──────────────────────────────────────────────────────┘
```

## 6. 已实现的文件

| 文件 | 内容 |
|---|---|
| `proto/common.proto` | FileInfo 新增 `block_size`(13)、`is_complete`(14) |
| `src/common/proto_utils.h` | `InodeToProto` / `ProtoToInode` 已补全新字段 |
| `src/master/inode_entry.h` | InodeEntry 结构体、OwnerGroupDict 字典、序列化/反序列化、Key 编解码（头文件实现） |
| `tests/master/inode_entry_test.cpp` | 24 个单元测试（OwnerGroupDict 字典、序列化往返、Key 编解码、有序性、前缀扫描、容错） |
| `CMakeLists.txt` | master_test 添加 inode_entry_test.cpp；gtest 优先 find_package |
