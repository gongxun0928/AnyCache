# 元数据设计方案

> 状态: Draft
> 日期: 2026-02-10
> 作者: AnyCache Team

## 1. 概述

本文档描述 AnyCache 的元数据设计，包括：

- 目录树（InodeTree）的内存实现
- Worker 端 RocksDB 的 KV 存储结构
- 文件信息的存储与传输方式
- 块位置追踪机制

**核心设计思想**：复合 BlockId 使文件→块映射零存储，所有 BlockId 由 `(inode_id, size, block_size)` 三元组推导而来。

## 2. 目录树 — InodeTree

### 2.1 数据结构

目录树以纯内存的 `std::unordered_map<InodeId, Inode>` 存储（`src/master/inode_tree.h`）：

```cpp
struct Inode {
  InodeId id = kInvalidInodeId;
  InodeId parent_id = kInvalidInodeId;
  std::string name;
  bool is_directory = false;
  uint64_t size = 0;
  uint32_t mode = 0644;
  std::string owner;
  std::string group;
  size_t block_size = kDefaultBlockSize;  // 文件级别块大小，默认 64MB
  int64_t creation_time_ms = 0;
  int64_t modification_time_ms = 0;

  // 仅目录有效：子名字 → 子 InodeId
  std::unordered_map<std::string, InodeId> children;

  // 文件是否写完（CreateFile 后为 false，CompleteFile 后变 true）
  bool is_complete = true;
};
```

### 2.2 字段说明

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | `uint64_t` | 全局唯一 InodeId，单调递增，从 2 开始分配（1 = 根目录） |
| `parent_id` | `uint64_t` | 父目录的 InodeId，根目录的 parent_id = kInvalidInodeId (0) |
| `name` | `string` | 文件/目录名（不含路径前缀） |
| `is_directory` | `bool` | 目录为 true，文件为 false |
| `size` | `uint64_t` | 文件大小（字节），目录为 0 |
| `mode` | `uint32_t` | Unix 权限，文件默认 0644，目录默认 0755 |
| `owner` / `group` | `string` | 文件属主和属组 |
| `block_size` | `size_t` | 文件级别块大小，默认 64MB，每个文件可不同 |
| `creation_time_ms` | `int64_t` | 创建时间戳（毫秒） |
| `modification_time_ms` | `int64_t` | 最后修改时间戳（毫秒） |
| `children` | `map<string, InodeId>` | 目录的子条目映射 |
| `is_complete` | `bool` | 文件写入是否完成 |

### 2.3 目录树组织方式

```
                   Inode(id=1, "/", is_directory=true)
                   children: {"data" → 2, "models" → 5}
                        /                      \
    Inode(id=2, "data", is_directory=true)    Inode(id=5, "models", ...)
    children: {"train.csv" → 3, "test" → 4}
              /                    \
  Inode(id=3, "train.csv",     Inode(id=4, "test",
        is_directory=false,          is_directory=true,
        size=200MB)                  children: {...})
```

- **路径解析**：`GetInodeByPath("/data/train.csv")` 从根 inode 出发，逐级在 `children` map 中查找 `"data"` → `"train.csv"`
- **ID 分配**：原子递增 `next_id_`，**不复用**，避免删除后旧 BlockId 冲突
- **并发控制**：`std::shared_mutex`，读操作共享锁，写操作独占锁

### 2.4 主要操作

| 操作 | 方法 | 说明 |
|---|---|---|
| 路径查找 | `GetInodeByPath(path)` | 从根逐级解析，返回目标 Inode |
| ID 查找 | `GetInodeById(id)` | 直接在 map 中查找 |
| 创建文件 | `CreateFile(path, mode)` | 父目录必须存在，分配新 InodeId |
| 创建目录 | `CreateDirectory(path, mode, recursive)` | 支持递归创建父目录 |
| 完成文件 | `CompleteFile(id, size)` | 设置 `is_complete=true` 和最终 size |
| 删除 | `Delete(path, recursive)` | 递归删除子树（如果是目录） |
| 重命名 | `Rename(src, dst)` | 移动到新的父目录并改名 |
| 列目录 | `ListDirectory(path)` | 返回目录下所有子 Inode |
| 更新大小 | `UpdateSize(id, new_size)` | FUSE 写入时更新文件大小 |

## 3. RocksDB 中的 KV 存储结构

### 3.1 使用范围

**当前 Master 的元数据全部在内存中，不使用 RocksDB。RocksDB 仅用于 Worker 节点持久化块元数据（BlockMeta）。**

### 3.2 Worker 端 BlockMeta 的 KV 结构

使用 RocksDB 默认 Column Family：

| 项目 | 格式 |
|---|---|
| **Key** | 8 字节**大端编码**的 `BlockId`（`uint64_t`） |
| **Value** | `BlockMeta` 结构体的二进制 `memcpy` 序列化 |

#### Key 编码

```cpp
// 8 字节大端编码，保证 RocksDB 中按 BlockId 有序排列
std::string RocksMetaStore::MakeKey(BlockId id) const {
  std::string key(8, '\0');
  for (int i = 7; i >= 0; --i) {
    key[i] = static_cast<char>(id & 0xFF);
    id >>= 8;
  }
  return key;
}
```

#### Value 结构（BlockMeta）

```cpp
struct BlockMeta {
  BlockId block_id;              // 复合 BlockId
  uint64_t length;               // 块的实际数据长度（最后一块可能不满）
  TierType tier;                 // 存储层级 (MEMORY/SSD/HDD)
  int64_t create_time_ms;        // 创建时间
  int64_t last_access_time_ms;   // 最近访问时间
  uint64_t access_count;         // 访问计数（用于淘汰策略）
};
```

序列化方式为 `memcpy` 整个结构体：

```cpp
std::string BlockMeta::Serialize() const {
  std::string buf(sizeof(BlockMeta), '\0');
  std::memcpy(buf.data(), this, sizeof(BlockMeta));
  return buf;
}
```

### 3.3 Key 有序性

由于 BlockId 的布局是 `[InodeId(40位) | BlockIndex(24位)]`，大端编码后在 RocksDB 中的排序天然按 **先 InodeId 再 BlockIndex** 排列：

```
key 排序示例（文件 inode_id=42）：
  MakeBlockId(42, 0) → 0x0000002A_000000  ← 第 0 块
  MakeBlockId(42, 1) → 0x0000002A_000001  ← 第 1 块
  MakeBlockId(42, 2) → 0x0000002A_000002  ← 第 2 块
  MakeBlockId(43, 0) → 0x0000002B_000000  ← 下一个文件的第 0 块
```

同一文件的所有块在 RocksDB 中物理相邻，可高效做范围扫描。

### 3.4 RocksDB 配置

- 压缩算法：LZ4 (`rocksdb::kLZ4Compression`)
- 小值优化：`OptimizeForSmallDb()`
- 最大打开文件数：256
- 存储路径：`{worker_data_dir}/meta`

### 3.5 CRUD 操作

| 操作 | 方法 | 说明 |
|---|---|---|
| 写入 | `PutBlockMeta(id, meta)` | `db_->Put(key, meta.Serialize())` |
| 读取 | `GetBlockMeta(id, &meta)` | `db_->Get(key, &val)` → `Deserialize` |
| 删除 | `DeleteBlockMeta(id)` | `db_->Delete(key)` |
| 全量扫描 | `ScanAll(&out)` | 迭代器遍历，用于 Worker 重启恢复 |

## 4. 文件信息的存储与传输

### 4.1 内部存储（Master 内存）

文件信息存在 `Inode` 结构体中，**不存储块列表**。文件的所有 BlockId 由三个值推导：

```
block_count = ceil(size / block_size)
block_ids[i] = MakeBlockId(inode_id, i)    for i in 0..block_count-1
```

示例：

```
Inode { id: 42, size: 200MB, block_size: 64MB }

block_count = ceil(200MB / 64MB) = 4
block_ids  = [MakeBlockId(42, 0),   // 第 0 块
              MakeBlockId(42, 1),   // 第 1 块
              MakeBlockId(42, 2),   // 第 2 块
              MakeBlockId(42, 3)]   // 第 3 块（实际数据 8MB）
```

10 亿文件 × 平均 10 块 = 100 亿 BlockId 引用，隐式推导节省约 **80 GB** 内存。

### 4.2 RPC 传输（Proto FileInfo）

通过 gRPC 传输时使用 Protobuf 格式（`proto/common.proto`）：

```protobuf
message FileInfo {
    uint64 inode_id = 1;
    string name = 2;
    string path = 3;             // 完整路径（按需计算）
    bool is_directory = 4;
    uint64 size = 5;
    uint32 mode = 6;
    string owner = 7;
    string group = 8;
    int64 creation_time_ms = 9;
    int64 modification_time_ms = 10;
    repeated uint64 block_ids = 11;  // 按需计算填充
    uint64 parent_id = 12;
}
```

- `path` 字段：不存储在 Inode 中，传输时按需从根路径拼接
- `block_ids` 字段：不存储在 Inode 中，传输时按需从 `(inode_id, size, block_size)` 推导

类型转换通过 `src/common/proto_utils.h` 中的 `InodeToProto()` / `ProtoToInode()` 完成。

## 5. 块位置追踪 — BlockMaster

Master 通过 `BlockMaster`（`src/master/block_master.h`）追踪每个 Block 在哪些 Worker 上有副本：

```cpp
class BlockMaster {
  // block_id → [Worker 位置列表]
  std::unordered_map<BlockId, std::vector<BlockLocationInfo>> block_locations_;
  // worker_id → {block_id 集合}（反向索引）
  std::unordered_map<WorkerId, std::set<BlockId>> worker_blocks_;
};
```

```cpp
struct BlockLocationInfo {
  BlockId block_id;
  WorkerId worker_id;
  std::string worker_address;
  TierType tier;            // MEMORY / SSD / HDD
};
```

**双向索引**用途：

- `block_locations_`：Client 查询 Block 在哪些 Worker 上（读数据路径）
- `worker_blocks_`：Worker 下线时批量清理该 Worker 上的所有块位置

## 6. 整体元数据架构

```
┌──────────────────────────────────────────────────────────────┐
│                      Master (全内存)                          │
│                                                              │
│  InodeTree                        BlockMaster                │
│  ┌───────────────────┐           ┌───────────────────┐       │
│  │ map<InodeId,Inode> │           │ map<BlockId,       │       │
│  │                   │           │   [WorkerLocInfo]> │       │
│  │ Inode {           │           │                   │       │
│  │   id, parent_id   │           │ map<WorkerId,     │       │
│  │   name, size      │  推导     │   set<BlockId>>   │       │
│  │   block_size      │ ──→      │                   │       │
│  │   mode, owner     │ BlockId   │                   │       │
│  │   children (dir)  │           │                   │       │
│  │   is_complete     │           │                   │       │
│  │ }                 │           │                   │       │
│  └───────────────────┘           └───────────────────┘       │
│                                                              │
│  ⚠ 当前无持久化（Journal 基础设施已有但未集成）                   │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│                      Worker (RocksDB)                         │
│                                                              │
│  MetaStore (RocksDB / InMemory fallback)                     │
│  ┌──────────────────────────────────────┐                    │
│  │ Key:   8 字节大端 BlockId            │                    │
│  │ Value: BlockMeta 二进制序列化         │                    │
│  │                                      │                    │
│  │ BlockMeta {                          │                    │
│  │   block_id         (uint64)          │                    │
│  │   length           (uint64)          │                    │
│  │   tier             (TierType)        │                    │
│  │   create_time_ms   (int64)           │                    │
│  │   last_access_ms   (int64)           │                    │
│  │   access_count     (uint64)          │                    │
│  │ }                                    │                    │
│  └──────────────────────────────────────┘                    │
│                                                              │
│  ✅ 重启后通过 ScanAll() 恢复缓存索引                          │
└──────────────────────────────────────────────────────────────┘
```

## 7. 当前状态总结

| 组件 | 存储方式 | 持久化 | 说明 |
|---|---|---|---|
| 目录树 (InodeTree) | 内存 `unordered_map` | ❌ | 重启丢失，Journal 基础设施已有但未集成 |
| 块位置 (BlockMaster) | 内存双向 map | ❌ | Worker 心跳/注册时可重建 |
| Worker 块元数据 (MetaStore) | RocksDB | ✅ | Key=大端 BlockId, Value=二进制 BlockMeta |
| RPC 传输 (FileInfo) | Protobuf | N/A | Inode ↔ FileInfo 转换通过 proto_utils.h |

## 8. 相关源文件

| 文件 | 说明 |
|---|---|
| `src/master/inode_tree.h/.cpp` | 目录树实现 |
| `src/master/block_master.h/.cpp` | 块位置追踪 |
| `src/worker/meta_store.h/.cpp` | Worker RocksDB 元数据存储 |
| `src/common/types.h` | 复合 BlockId 定义与辅助函数 |
| `src/common/proto_utils.h` | Inode ↔ Proto 类型转换 |
| `proto/common.proto` | FileInfo、BlockLocation 等消息定义 |
| `proto/master.proto` | Master gRPC 服务定义 |
| `design/block-id-and-file-layout.md` | Block ID 与文件-块映射设计（关联文档） |
