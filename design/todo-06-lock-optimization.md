# TODO-06: 锁策略优化

> 状态: 待确认
> 优先级: P2（可后续迭代）
> 依赖: TODO-02（Master 元数据持久化）

## 目标

优化 InodeTree 的锁策略，减少 RocksDB 写入对读操作的阻塞。

## 现状

当前使用单个 `shared_mutex`：

```cpp
mutable std::shared_mutex mu_;
```

- 读操作：shared_lock，~100ns
- 写操作：unique_lock，引入 RocksDB 后可达 ~100μs+
- 写操作持有独占锁期间，所有读操作被阻塞

## 方案对比

| 方案 | 描述 | 优点 | 缺点 |
|---|---|---|---|
| A. 保持现状 | 单 shared_mutex | 简单，正确性容易保证 | 写阻塞读 |
| B. 锁外持久化 | 持久化在锁外，只在更新内存时短暂加锁 | 读几乎不受影响 | 写路径需二次校验，复杂度增加 |
| C. 细粒度锁 | 每个 inode 独立锁 | 并发度最高 | 死锁风险，实现复杂 |

## 推荐方案 B：锁外持久化

### 写操作流程

```
1. shared_lock   → 校验前置条件（路径存在、名称不冲突等）
                   → 生成 new_id、构造 Inode
2. unlock
3. 构造 WriteBatch → 写 RocksDB（耗时，但不持锁）
4. unique_lock   → 再次校验（防止并发竞争改变了前置条件）
                   → 更新内存
5. unlock
```

### 示例：CreateFile

```cpp
Status InodeTree::CreateFile(const std::string &path, uint32_t mode,
                             InodeId *out_id) {
  auto parts = SplitPath(path);

  InodeId parent_id;
  InodeId new_id;
  Inode new_inode;

  // ① 共享锁：校验 + 准备
  {
    std::shared_lock lock(mu_);
    RETURN_IF_ERROR(ResolveParentLocked(parts, &parent_id));
    auto &parent = inodes_.at(parent_id);
    if (parent.children.count(parts.back()))
      return Status::AlreadyExists(...);

    new_id = AllocateId();
    new_inode = MakeInode(new_id, parent_id, parts.back(), mode);
  }

  // ② 锁外：持久化
  if (store_) {
    rocksdb::WriteBatch batch;
    store_->BatchPutInode(&batch, new_inode);
    store_->BatchPutEdge(&batch, parent_id, parts.back(), new_id);
    store_->BatchPutNextId(&batch, next_id_.load());
    RETURN_IF_ERROR(store_->WriteBatch(&batch));
  }

  // ③ 独占锁：再次校验 + 更新内存
  {
    std::unique_lock lock(mu_);
    // 再次校验：并发的另一个 CreateFile 可能先到
    auto pit = inodes_.find(parent_id);
    if (pit == inodes_.end())
      return Status::NotFound("parent removed concurrently");
    if (pit->second.children.count(parts.back())) {
      // 竞争失败，需要回滚 RocksDB（删除刚写入的数据）
      if (store_) {
        rocksdb::WriteBatch rollback;
        store_->BatchDeleteInode(&rollback, new_id);
        store_->BatchDeleteEdge(&rollback, parent_id, parts.back());
        store_->WriteBatch(&rollback);  // best-effort rollback
      }
      return Status::AlreadyExists(...);
    }

    pit->second.children[parts.back()] = new_id;
    inodes_[new_id] = std::move(new_inode);
  }

  *out_id = new_id;
  return Status::OK();
}
```

### 复杂度分析

- **优点**：RocksDB 写入（~100μs）不持锁，读操作几乎不受影响
- **缺点**：
  - 写路径需要二次校验
  - 竞争失败时需要回滚 RocksDB（额外写入）
  - 代码复杂度明显增加

### 建议

**先用方案 A（TODO-04 中的简单版本），后续根据实际性能瓶颈再切换到方案 B。**

理由：
- 缓存系统的元数据写操作频率通常不高（文件创建/删除远少于数据读写）
- unique_lock 持有 ~100μs 级别，对大多数场景可接受
- 过早优化增加 bug 风险

## 涉及文件

| 文件 | 变更 |
|---|---|
| `src/master/inode_tree.cpp` | 所有写操作的锁策略改造 |

## 验证方式

- 并发压力测试：多线程同时读写，验证数据一致性
- 性能对比：方案 A vs 方案 B 在不同读写比下的吞吐量
