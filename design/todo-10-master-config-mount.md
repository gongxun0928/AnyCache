# TODO-10: Master 传入完整 Config 以支持 MountTable

> 状态: 已完成
> 优先级: P3
> 依赖: 无

## 目标

使 MountTable 在解析挂载点（如 `s3://bucket/prefix`）时能使用完整 Config（含 S3 配置），从而正确创建 S3 UFS 实例。

## 现状

- `MasterServer` 仅接收 `MasterConfig`，`MountTable` 使用 `Config::Default()` 构造
- MountTable 在 `LoadFromDb` 时调用 `UfsFactory::Create(ufs_uri, config_)`，但 `config_` 为默认值，无 S3 凭证等
- 挂载 `s3://` 时，UfsFactory 无法创建有效 S3 UFS（且当前 factory 对 s3 返回 nullptr）

## 实现任务

1. **MasterServer 接收完整 Config**
   - `MasterServer` 增加接受 `Config` 的构造方式（或从 `MasterConfig` 扩展出包含 s3 等字段）
   - 建议：`MasterServer` 新增 `MasterServer(const Config& cfg)`，内部使用 `cfg.master` 与 `cfg` 传给 MountTable

2. **MountTable 使用传入的 Config**
   - `MountTable` 构造时接收 `Config`（已有 `MountTable(const Config&)`）
   - `MasterServer` 构造 MountTable 时传入 `cfg`：`mount_table_(std::make_unique<MountTable>(cfg))`

3. **main 入口**
   - `tools/anycache_master_main.cpp` 已加载 `Config`，将 `cfg` 传入 `MasterServer` 而非仅 `cfg.master`
   - 或新增 `MasterServer(cfg)` 重载

4. **测试**
   - 挂载 `s3://` 后 Resolve 能返回非空 UFS（需 TODO-07 配合）

## 参考

- `src/master/master_server.cpp`：MasterServer 构造
- `src/master/mount_table.cpp`：MountTable::LoadFromDb、UfsFactory::Create
