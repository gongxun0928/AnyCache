# TODO-02: Master 元数据持久化

> 状态: 已完成
> 优先级: P0
> 依赖: TODO-01（已完成）

## 1. 两级存储模型

### 1.1 问题

文件数量可能非常大（亿级），将所有 Inode 全量加载到内存不现实。但目录数量远小于文件数量（通常差 2~3 个数量级），且路径解析必须走内存才能保证性能。

### 1.2 方案

**目录常驻内存，文件按需查询 RocksDB。**

| 类别 | 内存 | RocksDB | 说明 |
|---|---|---|---|
| 目录 Inode | ✅ 常驻（含 children map） | ✅ 持久化 | 路径解析全程在内存 |
| 文件 Inode | ❌ 不加载 | ✅ 持久化 | 按需点查 RocksDB |
| Edge | ✅ 体现在目录的 children map | ✅ 持久化 | 恢复时重建 children |
| next_id | ✅ 内存计数器 | ✅ 批量持久化 | 每 1000 个 ID 持久化一次 |

### 1.3 内存占用估算

| 场景 | 全量加载 | 两级存储（仅目录） |
|---|---|---|
| 1 亿文件 + 10 万目录 | ~30 GB | ~30 MB |
| 10 亿文件 + 100 万目录 | ~300 GB | ~300 MB |

## 2. InodeStore — RocksDB 持久化层

### 2.1 接口

```cpp
class InodeStore {
public:
  InodeStore() = default;
  ~InodeStore();

  Status Open(const std::string &db_path);
  Status Close();

  // ─── 运行时读操作 ──────────────────────────────

  // 点查单个 Inode（name 从 InodeEntry 变长部分恢复）
  Status GetInode(InodeId id, Inode *out);

  // 批量点查多个 Inode（ListDirectory 优化）
  Status MultiGetInodes(const std::vector<InodeId> &ids,
                        std::vector<Inode> *out);

  // 读取 next_id 计数器
  Status GetNextId(InodeId *out);

  // ─── 原子写操作 ────────────────────────────────

  Status CommitBatch(rocksdb::WriteBatch *batch);

  void BatchPutInode(rocksdb::WriteBatch *batch, InodeId id, const Inode &inode);
  void BatchDeleteInode(rocksdb::WriteBatch *batch, InodeId id);
  void BatchPutEdge(rocksdb::WriteBatch *batch, InodeId parent_id,
                    const std::string &child_name, InodeId child_id);
  void BatchDeleteEdge(rocksdb::WriteBatch *batch, InodeId parent_id,
                       const std::string &child_name);
  void BatchPutNextId(rocksdb::WriteBatch *batch, InodeId next_id);

  // ─── 恢复接口 ──────────────────────────────────

  // 扫描 inodes CF，仅返回目录 Inode（跳过文件）
  Status ScanDirectoryInodes(std::vector<Inode> *out);

  // 全量扫描 edges CF
  Status ScanAllEdges(
      std::vector<std::tuple<InodeId, std::string, InodeId>> *out);

private:
  void MaybePersistDict(rocksdb::WriteBatch *batch);

  std::unique_ptr<rocksdb::DB> db_;
  rocksdb::ColumnFamilyHandle *cf_inodes_ = nullptr;
  rocksdb::ColumnFamilyHandle *cf_edges_ = nullptr;
  OwnerGroupDict dict_;  // owner/group 字典编码
};
```

### 2.2 RocksDB 配置

```cpp
// inodes CF: 点查为主 → bloom filter
rocksdb::BlockBasedTableOptions inodes_table_opts;
inodes_table_opts.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
inodes_cf_opts.table_factory.reset(
    rocksdb::NewBlockBasedTableFactory(inodes_table_opts));

// edges CF: 前缀扫描为主 → prefix extractor (ParentId = 8B)
edges_cf_opts.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(8));

// 压缩: 运行时检测可用算法 (LZ4 → Snappy → NoCompression)
```

### 2.3 关键实现

**Open 流程（含字典加载）：**

打开 RocksDB（含 default / inodes / edges 三个 CF），然后从 `kOwnerDictKey` / `kGroupDictKey` 特殊 key 加载 owner/group 字典到内存。

**BatchPutInode 中字典持久化：**

序列化 Inode 时调用 `dict_.GetOrAddOwnerId(owner)`，如字典有新增条目（`dict_.IsDirty()`），随同一个 WriteBatch 将字典持久化到 RocksDB。

**MultiGetInodes：**

使用 RocksDB `MultiGet` API，构造重复 CF handle 的 vector 进行批量点查，跳过不存在的 key。

**ScanDirectoryInodes（快速恢复核心）：**

顺序扫描 inodes CF，跳过特殊 key（`k >= kOwnerDictKey`），仅检查 flags 字节（offset 44）的 `kInodeEntryFlagDirectory` 位，只反序列化目录 Inode。

**ScanAllEdges：**

使用 `total_order_seek = true` 绕过 prefix extractor 进行全量顺序扫描。

## 3. InodeTree 重构

### 3.1 类结构

```cpp
class InodeTree {
public:
  InodeTree();

  void SetStore(InodeStore *store);
  Status Recover();

  // ─── 读操作 ─────────────────────────────────────
  Status GetInodeByPath(const std::string &path, Inode *out) const;
  Status GetInodeById(InodeId id, Inode *out) const;
  Status ListDirectory(const std::string &path,
                       std::vector<Inode> *children) const;

  // ─── 写操作 ─────────────────────────────────────
  Status CreateFile(const std::string &path, uint32_t mode, InodeId *out_id);
  Status CreateDirectory(const std::string &path, uint32_t mode,
                         bool recursive, InodeId *out_id);
  Status CompleteFile(InodeId id, uint64_t size);
  Status Delete(const std::string &path, bool recursive);
  Status Rename(const std::string &src, const std::string &dst);
  Status UpdateSize(InodeId id, uint64_t new_size);

  size_t DirCount() const;
  InodeId GetRootId() const { return root_id_; }

private:
  static std::vector<std::string> SplitPath(const std::string &path);
  Status ResolvePathLocked(const std::string &path, InodeId *id) const;
  InodeId AllocateId();
  void CollectSubtreeForDeletion(
      InodeId dir_id, std::vector<std::pair<InodeId, std::string>> *edges,
      std::vector<InodeId> *inode_ids, std::vector<InodeId> *dir_ids) const;
  void RemoveDirSubtreeFromMemory(InodeId id);

  mutable std::shared_mutex mu_;
  std::unordered_map<InodeId, Inode> dir_inodes_;  // 仅目录
  InodeId root_id_ = 1;
  std::atomic<InodeId> next_id_{2};

  InodeStore *store_ = nullptr;
  static constexpr InodeId kIdAllocBatchSize = 1000;
  InodeId alloc_end_ = 2;
};
```

**两种运行模式：**

- `store_ == nullptr`（纯内存模式）：所有 Inode（含文件）在 `dir_inodes_` 中，无持久化。用于单元测试和向后兼容。
- `store_ != nullptr`（两级存储模式）：目录在 `dir_inodes_`，文件通过 `store_` 按需查询。

### 3.2 读操作

**GetInodeByPath("/a/b/c/file.txt")：**

```
① 路径解析（全程在 dir_inodes_ 内存中）:
   root → children["a"] → id_a → children["b"] → id_b
       → children["c"] → id_c → children["file.txt"] → id_file

② 获取 Inode:
   if id_file in dir_inodes_ → 直接返回（目录）
   else if store_ → store_->GetInode(id_file)
```

中间节点必须是目录（在 `dir_inodes_` 中），最后一个节点可以是文件或目录。

**GetInodeById(id)：**

```
① if id in dir_inodes_ → 直接返回（目录）
② else if store_ → store_->GetInode(id)
```

name 从 InodeEntry 变长部分直接恢复，无需反查 edges CF。

**ListDirectory("/a/b/c")：**

```
① 路径解析到 dir_inodes_[id_c]（内存）
② 遍历 children:
   - dir_children: 在 dir_inodes_ 中的 → 直接从内存取
   - file_ids: 不在 dir_inodes_ 中的 → 收集 InodeId 列表
③ store_->MultiGetInodes(file_ids) 批量点查文件属性
④ 合并返回
```

### 3.3 写操作

所有写操作遵循 **RocksDB 先写 → 成功后再更新内存**。

| 操作 | WriteBatch 内容 | 内存更新 |
|---|---|---|
| CreateFile | PutInode + PutEdge | 父目录 children += new_id（文件不加入 dir_inodes_） |
| CreateDirectory | PutInode + PutEdge | 父目录 children += new_id，dir_inodes_ += new_dir |
| CompleteFile | GetInode → 修改 → PutInode | 无（文件不在内存） |
| UpdateSize | GetInode → 修改 → PutInode | 无（文件不在内存） |
| Delete | DeleteInode + DeleteEdge（递归时含子树） | 父目录 children -= name，dir_inodes_ -= 目录子树 |
| Rename | PutInode + DeleteEdge(old) + PutEdge(new) | 两个父目录 children 更新，dir_inodes_ 更新 name/parent_id |

### 3.4 启动恢复

```cpp
Status InodeTree::Recover() {
  if (!store_) return Status::OK();
  std::unique_lock lock(mu_);
  dir_inodes_.clear();

  // ① 仅加载目录 Inode（name 从 InodeEntry 变长部分恢复）
  std::vector<Inode> dirs;
  RETURN_IF_ERROR(store_->ScanDirectoryInodes(&dirs));
  for (auto &d : dirs) {
    dir_inodes_[d.id] = std::move(d);
  }

  // ② 加载所有 Edge → 填充目录的 children map
  std::vector<std::tuple<InodeId, std::string, InodeId>> edges;
  RETURN_IF_ERROR(store_->ScanAllEdges(&edges));
  for (auto &[parent_id, name, child_id] : edges) {
    auto it = dir_inodes_.find(parent_id);
    if (it != dir_inodes_.end()) {
      it->second.children[name] = child_id;
    }
  }

  // ③ 恢复 next_id（从批量预分配上界恢复）
  InodeId stored_next_id = 0;
  if (store_->GetNextId(&stored_next_id).ok() && stored_next_id > 0) {
    next_id_.store(stored_next_id);
    alloc_end_ = stored_next_id;
  } else {
    InodeId max_id = 1;
    for (auto &[id, _] : dir_inodes_) {
      max_id = std::max(max_id, id);
    }
    next_id_.store(max_id + 1);
    alloc_end_ = max_id + 1;
  }

  // ④ 首次启动：创建并持久化根目录
  if (dir_inodes_.find(root_id_) == dir_inodes_.end()) {
    Inode root{};
    root.id = root_id_;
    root.parent_id = kInvalidInodeId;
    root.is_directory = true;
    root.mode = 0755;
    root.creation_time_ms = NowMs();
    root.modification_time_ms = root.creation_time_ms;

    rocksdb::WriteBatch batch;
    store_->BatchPutInode(&batch, root_id_, root);
    RETURN_IF_ERROR(store_->CommitBatch(&batch));
    dir_inodes_[root_id_] = std::move(root);
  }

  return Status::OK();
}
```

| 步骤 | 操作 | I/O 类型 | 数据量 |
|---|---|---|---|
| ① ScanDirectoryInodes | 顺序扫描 inodes CF，仅反序列化目录 | 顺序读 | 全量扫描，仅保留目录 |
| ② ScanAllEdges | 顺序扫描 edges CF | 顺序读 | 全量 |
| ③ GetNextId | 点查 | 点读 | 1 条 |
| ④ 创建根目录 | 仅首次启动 | 写 | 1 条 |

对于 1 亿文件 + 10 万目录的场景，预计恢复时间 **~1~2 秒**（现代 SSD 顺序读 ~2GB/s）。

### 3.5 next_id 批量预分配

```cpp
InodeId InodeTree::AllocateId() {
  InodeId id = next_id_.fetch_add(1);
  if (store_ && id >= alloc_end_) {
    alloc_end_ = id + kIdAllocBatchSize;  // kIdAllocBatchSize = 1000
    rocksdb::WriteBatch batch;
    store_->BatchPutNextId(&batch, alloc_end_);
    store_->CommitBatch(&batch);  // best-effort
  }
  return id;
}
```

每 1000 个 ID 持久化一次上界。重启后从持久化的上界恢复，最多浪费 999 个 ID（40 位 InodeId 空间下可忽略）。

## 4. 启动流程

### 4.1 MasterConfig

```cpp
struct MasterConfig {
  std::string host = "0.0.0.0";
  int port = 19999;
  std::string journal_dir = "/tmp/anycache/journal";
  int worker_heartbeat_timeout_ms = 30000;
  std::string meta_db_dir = "/tmp/anycache/master/meta";
};
```

### 4.2 FileSystemMaster

```cpp
class FileSystemMaster {
public:
  explicit FileSystemMaster(const MasterConfig &config);
  Status Init();   // 打开 RocksDB + 恢复 InodeTree

private:
  MasterConfig config_;
  std::unique_ptr<InodeStore> inode_store_;
  InodeTree inode_tree_;
  BlockMaster block_master_;
  WorkerManager worker_mgr_;
};

Status FileSystemMaster::Init() {
  inode_store_ = std::make_unique<InodeStore>();
  RETURN_IF_ERROR(inode_store_->Open(config_.meta_db_dir));

  inode_tree_.SetStore(inode_store_.get());
  RETURN_IF_ERROR(inode_tree_.Recover());

  return Status::OK();
}
```

`MasterServer::Start()` 中调用 `fs_master_->Init()`，在启动 gRPC 服务之前完成恢复。

### 4.3 配置文件

```yaml
master:
  host: "0.0.0.0"
  port: 19999
  journal_dir: "/tmp/anycache/journal"
  meta_db_dir: "/var/lib/anycache/master/meta"
  heartbeat_timeout_ms: 30000
```

## 5. 各操作的完整读写路径

| 操作 | 内存读 | RocksDB 读 | RocksDB 写 | 内存写 |
|---|---|---|---|---|
| GetInodeByPath(目录) | 路径解析 + 返回 | — | — | — |
| GetInodeByPath(文件) | 路径解析 | GetInode 点查 | — | — |
| GetInodeById(目录) | 直接返回 | — | — | — |
| GetInodeById(文件) | — | GetInode 点查 | — | — |
| ListDirectory | 遍历 children | MultiGetInodes 批量查文件 | — | — |
| CreateFile | 解析父目录 | — | WriteBatch | 父目录 children |
| CreateDirectory | 解析父目录 | — | WriteBatch | 父目录 children + dir_inodes_ |
| CompleteFile | — | GetInode 读当前值 | WriteBatch | — |
| UpdateSize | — | GetInode 读当前值 | WriteBatch | — |
| Delete | 路径解析 + 收集子树 | — | WriteBatch | 移除 children + dir_inodes_ |
| Rename | 解析 src/dst 父目录 | — | WriteBatch | 更新 children + dir_inodes_ |

## 6. 已实现的文件

| 文件 | 内容 |
|---|---|
| `src/master/inode_store.h` | InodeStore 类声明 |
| `src/master/inode_store.cpp` | InodeStore 实现（Open/Close、读写、恢复扫描） |
| `src/master/inode_tree.h` | InodeTree 重构：`dir_inodes_`、SetStore/Recover、DirCount |
| `src/master/inode_tree.cpp` | 所有操作改为两级存储模型，支持 store=nullptr 纯内存回退 |
| `src/master/file_system_master.h` | 新增 `InodeStore` 成员、`Init()` 方法 |
| `src/master/file_system_master.cpp` | 实现 Init() |
| `src/master/master_server.cpp` | `Start()` 中调用 `fs_master_->Init()` |
| `src/common/config.h` | `MasterConfig` 新增 `meta_db_dir` |
| `src/common/config.cpp` | YAML 解析 `meta_db_dir` 字段 |
| `config/example.yaml` | 新增 `meta_db_dir` 配置项 |
| `CMakeLists.txt` | `anycache_master` 链接 RocksDB，添加 `inode_store.cpp`，测试添加 `inode_store_test.cpp` |
| `tests/master/inode_store_test.cpp` | 11 个 InodeStore 单测 + 10 个 InodeTree+Store 集成测试 |

## 7. 测试覆盖

全部 65 个测试通过（`master_test`）。

**InodeStore 单元测试（11 个）：**

- PutInode → GetInode 往返（含 name/owner/group 恢复）
- GetInode 未找到返回 NotFound
- MultiGetInodes 批量查询（含不存在的 key 被跳过）
- MultiGetInodes 空列表
- DeleteInode 删除后 NotFound
- ScanDirectoryInodes 正确过滤目录/文件
- ScanAllEdges 完整返回
- next_id 持久化往返
- next_id 新 DB 返回 NotFound
- OwnerGroupDict 跨重启持久化与恢复
- DeleteEdge 删除后扫描验证

**InodeTree + Store 集成测试（10 个）：**

- 恢复后根目录存在
- 创建文件 → 重启 → 文件可从 RocksDB 查到
- 创建目录 → 重启 → 目录在内存
- 递归创建目录 → 重启 → 完整目录树恢复
- ListDirectory 混合目录+文件 → 重启 → 结果正确
- CompleteFile → 重启 → size 和 is_complete 正确
- 删除文件 → 重启 → 数据已清除
- 递归删除目录 → 重启 → 子树全部清除
- 重命名文件 → 重启 → 新路径可访问
- GetInodeById 分别查询文件和目录

**InodeTree 纯内存测试（10 个）：** 原有测试全部通过，确认 store=nullptr 向后兼容。
