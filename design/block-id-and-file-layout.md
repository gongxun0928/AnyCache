# Block ID 与文件-块映射设计

> 状态: Draft
> 日期: 2026-02-10
> 作者: AnyCache Team

## 1. 设计目标

- Block ID 包含完整语义：给定任意 Block ID，可直接提取所属文件和块位置，无需查表
- 文件→块映射零存储：给定文件 ID 和文件大小，可枚举所有 Block ID，无需维护块列表
- Block ID 由客户端本地计算，Master 不在写数据路径上，消除全局分配瓶颈
- 支撑**数十亿文件、百亿级块**的规模

## 2. 复合 Block ID

### 2.1 位布局

将 `(InodeId, BlockIndex)` 编码到一个 `uint64_t` 中：

```
uint64_t BlockId (64 bits)
┌─────────────────────────────┬──────────────────────┐
│      InodeId (40 bits)      │  BlockIndex (24 bits) │
│      bits [63:24]           │  bits [23:0]          │
└─────────────────────────────┴──────────────────────┘
```

### 2.2 容量

| 字段 | 位数 | 范围 | 含义 |
|---|---|---|---|
| InodeId | 40 | 0 ~ 1,099,511,627,775 | 约 **1.1 万亿**个唯一文件/目录 |
| BlockIndex | 24 | 0 ~ 16,777,215 | 每文件最多 **1677 万**个块 |

最大文件大小（默认 64MB block_size）：

```
16,777,216 × 64 MB = 1 PB
```

InodeId 耗尽时间（每秒创建 100 万个文件）：

```
1.1 × 10^12 / 10^6 / 86400 / 365 ≈ 34.8 年
```

两者对 AnyCache 的使用场景均绰绰有余。

### 2.3 保留值

```cpp
// BlockId = 0 保留为无效值
// InodeId = 0 保留为无效值
// InodeId = 1 保留为根目录
// BlockIndex = 0 为文件的第一个块
constexpr BlockId kInvalidBlockId = 0;
// MakeBlockId(0, 0) == 0 == kInvalidBlockId，天然满足
```

### 2.4 辅助函数

```cpp
namespace anycache {

constexpr int kBlockIndexBits = 24;
constexpr uint64_t kBlockIndexMask = (1ULL << kBlockIndexBits) - 1;  // 0xFFFFFF
constexpr InodeId kMaxInodeId = (1ULL << 40) - 1;
constexpr uint32_t kMaxBlockIndex = (1U << kBlockIndexBits) - 1;

// 由 InodeId 和文件内块索引合成 BlockId
inline BlockId MakeBlockId(InodeId inode_id, uint32_t block_index) {
  return (static_cast<uint64_t>(inode_id) << kBlockIndexBits)
       | (block_index & kBlockIndexMask);
}

// 从 BlockId 提取 InodeId
inline InodeId GetInodeId(BlockId block_id) {
  return block_id >> kBlockIndexBits;
}

// 从 BlockId 提取块索引
inline uint32_t GetBlockIndex(BlockId block_id) {
  return static_cast<uint32_t>(block_id & kBlockIndexMask);
}

// 计算文件包含的块数
inline uint32_t GetBlockCount(uint64_t file_size,
                              size_t block_size = kDefaultBlockSize) {
  if (file_size == 0) return 0;
  return static_cast<uint32_t>((file_size + block_size - 1) / block_size);
}

// 计算某个 block 的实际数据长度（最后一个块可能不满）
inline size_t GetBlockLength(uint64_t file_size, uint32_t block_index,
                             size_t block_size = kDefaultBlockSize) {
  uint64_t start = static_cast<uint64_t>(block_index) * block_size;
  if (start >= file_size) return 0;
  return static_cast<size_t>(std::min(static_cast<uint64_t>(block_size),
                                      file_size - start));
}

} // namespace anycache
```

## 3. 文件→块的隐式映射

Inode 不存储块列表，文件的所有 Block ID 由 `inode_id`、`file_size`、`block_size` 三个值推导得出：

```
Inode {
  id: 42
  size: 200MB
  block_size: 64MB
}

block_count = ceil(200MB / 64MB) = 4
block_ids  = [MakeBlockId(42, 0),   // 第 0 块
              MakeBlockId(42, 1),   // 第 1 块
              MakeBlockId(42, 2),   // 第 2 块
              MakeBlockId(42, 3)]   // 第 3 块（实际数据 8MB）
```

10 亿文件 × 平均 10 个块 = 100 亿 BlockId 引用，每个 8 字节 → 隐式推导节省约 **80 GB** 内存。

### 3.1 各操作的块映射方式

| 操作 | 实现 |
|---|---|
| 创建文件 | `CreateFile` → 分配 InodeId |
| 写入第 N 块 | `MakeBlockId(inode_id, N)` → 确定性生成，直接写入 Worker |
| 查询文件所有块 | `for i in 0..block_count: MakeBlockId(inode_id, i)` |
| 给定 block 查所属文件 | `GetInodeId(block_id)` → 直接位运算解码 |
| 给定 block 查文件内偏移 | `GetBlockIndex(block_id) * block_size` → 直接位运算解码 |

## 4. 各模块设计

### 4.1 Inode（`src/master/inode_tree.h`）

```cpp
struct Inode {
  InodeId id;
  uint64_t size;
  size_t block_size = kDefaultBlockSize;  // 文件级别块大小
  bool is_complete = true;
  // 无需 blocks 列表，块 ID 由 id + size + block_size 推导
};
```

Inode 上的操作仅需更新 `size`，不涉及块列表维护。

### 4.2 BlockMaster（`src/master/block_master.h`）

```cpp
class BlockMaster {
  // Block ID 由客户端本地计算，BlockMaster 不负责分配
  // 仅负责 block 位置追踪（用于缓存命中的快速查询）
  void AddBlockLocation(BlockId block_id, WorkerId worker_id, ...);
  Status GetBlockLocations(...);
};
```

Block → Worker 映射由一致性哈希计算（见第 5 节）。

### 4.3 BlockMeta（`src/worker/meta_store.h`）

```cpp
struct BlockMeta {
  BlockId block_id;
  // file_id 和 offset_in_file 不再单独存储
  // 可用 GetInodeId(block_id) 和 GetBlockIndex(block_id) * block_size 随时获取
  uint64_t length;           // 块的实际数据长度（最后一个块可能不满）
  TierType tier;
  int64_t create_time_ms;
  int64_t last_access_time_ms;
  uint64_t access_count;
};
```

### 4.4 写入流程（Client / FUSE）

Client 和 FUSE 层在写入时本地计算 Block ID，不经过 Master：

```
Client 本地计算: block_id = MakeBlockId(inode_id, block_index) → 直接写入 Worker
```

**Master 仅参与 CreateFile 和 CompleteFile**，不在数据写入路径上。

FUSE 写入示例（`src/fuse/fuse_operations.cpp`）：

```cpp
int Write(...) {
  uint32_t block_index = offset / block_size;
  BlockId block_id = MakeBlockId(inode_id, block_index);
  // 如果块不存在则创建，否则覆盖写
  ctx->block_store->EnsureBlock(block_id, block_size);
  ctx->block_store->WriteBlock(block_id, buf, size, offset_in_block);
  ctx->master->GetInodeTree().UpdateSize(inode_id, new_size);
}
```

## 5. 与一致性哈希的配合

复合 Block ID 天然适合一致性哈希放置：

```
文件 inode_id=42, 大小=200MB, block_size=64MB

Block 0: MakeBlockId(42, 0) = 0x0000002A_000000  → hash → Worker A, B, C
Block 1: MakeBlockId(42, 1) = 0x0000002A_000001  → hash → Worker D, E, F
Block 2: MakeBlockId(42, 2) = 0x0000002A_000002  → hash → Worker A, C, E
Block 3: MakeBlockId(42, 3) = 0x0000002A_000003  → hash → Worker B, D, F
```

- 同一文件的不同块哈希值不同 → **自动分散到不同 Worker**
- 实现数据条带化（striping），并行读写吞吐量随 Worker 数线性增长
- 无需额外的放置策略

## 6. 边界场景

### 6.1 文件追加写（Append）

```
原文件: size=128MB → block_count=2 (index 0, 1)
追加 50MB 后: size=178MB → block_count=3 (index 0, 1, 2)

新 block_id = MakeBlockId(inode_id, 2)  // 确定性，无需协调
```

- Block 0、1 的 block_id 不变，已有缓存仍然有效
- 只需写入新的 Block 2

### 6.2 文件覆盖写（Overwrite）

```
覆盖 offset=70MB, size=10MB
→ 落在 block_index=1 (64~128MB 范围), offset_in_block=6MB
→ block_id = MakeBlockId(inode_id, 1)  // 和创建时一样
→ WriteBlock(block_id, buf, 10MB, 6MB)
```

- 覆盖写不改变 block_id，只更新块内数据
- 多副本场景下需保证所有副本都被更新（或用版本号做 CoW）

### 6.3 文件截断（Truncate）

```
原文件: size=200MB → blocks 0,1,2,3
Truncate to 100MB → blocks 0,1 保留, blocks 2,3 作废

作废的 block_id:
  MakeBlockId(inode_id, 2) 和 MakeBlockId(inode_id, 3)
  → 通知 Worker 异步回收即可
```

### 6.4 InodeId 不复用

**InodeId 单调递增，不复用**：

- 删除文件后，其 InodeId 不回收，避免新旧文件 block_id 冲突
- 40 位空间（1.1 万亿）在每秒百万次创建下可用 34 年
- 如有需要，可后续引入 generation 机制（在 Inode 中增加 generation 字段，编入 block_id 高位）

### 6.5 可变块大小

每个文件可以有不同的 `block_size`（存在 Inode 元数据中）：

```cpp
struct Inode {
  size_t block_size = kDefaultBlockSize;  // 文件级别块大小
};
```

- 大文件用更大的块（减少块数量）
- 小文件用更小的块（减少空间浪费）
- block_index 的含义不变（第 N 个块），只是每个块的字节范围不同
