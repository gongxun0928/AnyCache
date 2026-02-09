# TODO-08: CacheBlock 从 UFS 拉取并缓存

> 状态: 已完成
> 优先级: P0
> 依赖: TODO-07（S3 UFS）、TODO-10（Worker 需 Config 创建 UFS）

## 目标

实现 Worker 的 `CacheBlock` 与 `AsyncCacheBlock` RPC，使 Client 或 Master 可指示 Worker 从 UFS（如 S3）拉取数据并写入 BlockStore，形成完整「按需缓存」链路。

## 现状

- `WorkerServiceImpl::CacheBlock`、`AsyncCacheBlock` 为 TODO
- `DataMover` 已实现 `SubmitPreload(block_id, ufs_path, offset, length)`：从 UFS 读入 BlockStore
- Worker 当前无 UFS 实例：需根据 `ufs_path` 解析 scheme 并创建 UFS（需 Config）

## 实现任务

1. **Worker 持有 Config 或 S3Config**
   - `WorkerServer` 构造时传入 `Config`（或至少 `S3Config`），供 UFS 创建使用
   - `tools/anycache_worker_main.cpp` 已加载 `Config`，需将 `cfg` 传入 `WorkerServer`

2. **CacheBlock 实现**
   - 解析 `CacheBlockRequest.ufs_path`（如 `s3://bucket/prefix/path`）
   - 使用 `UfsFactory::Create(ufs_path, config)` 创建 UFS
   - 调用 `DataMover::SubmitPreload` 或直接 `BlockStore::EnsureBlock` + UFS Read + Write
   - 完成后向 Master `ReportBlockLocation`（若尚未上报）

3. **AsyncCacheBlock**
   - 异步提交 preload 任务，立即返回
   - 后台线程或 DataMover 执行实际拉取

4. **测试**
   - 单元测试：Mock UFS 或 Local UFS
   - 集成测试：挂载 S3（MinIO / CubeFS），Client 触发 CacheBlock，验证 BlockStore 中有数据

## 参考

- `src/worker/worker_service_impl.cpp`：CacheBlock RPC 定义
- `src/worker/data_mover.cpp`：SubmitPreload 实现
- `proto/worker.proto`：CacheBlockRequest 字段
