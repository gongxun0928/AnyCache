# Plan: P0-P2 功能补齐

> Status: Done

## Goal

补齐 AnyCache 与 README 描述的最终定位之间的功能差距，按 P0→P1→P2 优先级实施。

## Steps

### P0 — 基本功能链路

1. **AsyncCacheBlock 实现** — `worker_service_impl.cpp`
   - 集成 DataMover 异步提交 UFS→BlockStore 预热任务
   - 需在 WorkerServiceImpl 中增加 DataMover 指针

2. **PersistBlock 实现** — `worker_service_impl.cpp`
   - 集成 DataMover 异步提交 BlockStore→UFS 持久化任务
   - 需在 WorkerServiceImpl 中增加 DataMover 指针

3. **FUSE truncate 实现**
   - `proto/master.proto` 增加 TruncateFile RPC
   - `FileSystemMaster` / `InodeTree` 增加 TruncateFile 方法
   - `MasterServiceImpl` 实现 TruncateFile
   - `FileSystemClient` 增加 TruncateFile 方法
   - `fuse_operations.cpp` 调用 FileSystemClient::TruncateFile

4. **Client Mount/Unmount + CLI 命令**
   - `FileSystemClient` 增加 Mount/Unmount/GetMountTable 方法
   - `anycache_cli_main.cpp` 增加 mount/unmount/mountTable 命令

### P1 — README 承诺兑现

5. **自动缓存层级晋升/淘汰** — `block_store.cpp`
   - ReadBlock 时热度达阈值自动晋升到更快层
   - 写入时空间不足自动触发淘汰

6. **Prometheus HTTP 端点** — `master_server.cpp` / `worker_server.cpp`
   - 使用独立线程启动简单 HTTP server 暴露 /metrics

7. **PageStore::InvalidateFile 真实实现** — `page_store.cpp`
   - 维护 file_id → PageKey 索引，按文件清除缓存

8. **CacheClient 完善** — `cache_client.cpp`
   - 支持跨 page 读取，不限于 "第一个 page"

### P2 — 生产就绪

9. **测试覆盖** — Client / FUSE / WorkerService 测试
10. **FUSE 扩展操作** — chown, utimens
11. **CLI 目录拷贝**

## Risks & Assumptions

- DataMover 当前设计为构造时传入单一 UFS 实例；AsyncCacheBlock 需支持动态 UFS → 需在 WorkerServiceImpl 新建 DataMover 或改造 DataMover 接口
- Prometheus HTTP 端点不引入额外依赖，使用 raw socket 实现简单 HTTP
- Truncate 仅修改元数据（size），不清理超出范围的 block 数据（简化实现）
