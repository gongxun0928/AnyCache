# Plan: UFS 集成与 CLI 完善

> Status: 待确认
> 关联 TODO: todo-07, todo-08, todo-09, todo-10, todo-11

## Goal

通过挂载类 S3 存储（MinIO / CubeFS）并打通 CacheBlock 链路，使 AnyCache 具备完整的「从 UFS 拉取 → 缓存 → 读写」能力；同时实现 CLI 的完整 fs 命令组（类似 Alluxio），支持创建、读取、写入等，无功能限制。暂不包含 HDFS、Master HA。

## 已确认

1. **测试环境**：MinIO 或 CubeFS 等可本地部署的类 S3 系统
2. **CacheBlock**：先实现同步版本，保证逻辑跑通
3. **CLI**：按文件系统需求实现完整 fs 命令组（cat、cp、head、ls、mkdir、mv、rm、stat、tail、touch、write、location、test），write 需支持完整创建与写入，无限制

## Steps

### Phase 1: 配置与基础设施（P3）

| 步骤 | 描述 | 文件 |
|------|------|------|
| 1.1 | Master 传入完整 Config | `tools/anycache_master_main.cpp`、`src/master/master_server.h/cpp` |
| 1.2 | MountTable 使用传入的 Config 创建 UFS | `src/master/master_server.cpp`（构造 MountTable 时传入 cfg） |
| 1.3 | 验证：MountTable 持有有效 Config，为后续 S3 挂载做准备 | - |

**产出**：MountTable 可正确创建需 S3 配置的 UFS（待 Phase 2 接入 S3 后验证）。

---

### Phase 2: S3 UFS 接入（P0）

| 步骤 | 描述 | 文件 |
|------|------|------|
| 2.1 | UfsFactory 中接入 S3 scheme | `src/ufs/ufs_factory.cpp` |
| 2.2 | S3UnderFileSystem 完整实现（AWS SDK） | `src/ufs/s3_ufs.cpp` |
| 2.3 | 单元/集成测试（MinIO 或 CubeFS 本地部署） | `tests/ufs/s3_ufs_test.cpp` |
| 2.4 | 更新 README、config/example.yaml 说明 | `README.md`、`config/example.yaml` |

**产出**：可挂载 `s3://bucket/prefix`（MinIO / CubeFS），MountTable 解析后返回有效 S3 UFS。

---

### Phase 3: CacheBlock 从 UFS 拉取（P0）

| 步骤 | 描述 | 文件 |
|------|------|------|
| 3.1 | WorkerServer 接收完整 Config | `tools/anycache_worker_main.cpp`、`src/worker/worker_server.h/cpp` |
| 3.2 | CacheBlock RPC 实现（同步） | `src/worker/worker_service_impl.cpp` |
| 3.3 | 解析 ufs_path → UfsFactory::Create → DataMover::SubmitPreload | 同上 |
| 3.4 | 集成测试：挂载 S3（MinIO/CubeFS），触发 CacheBlock，验证 BlockStore | 手动或 `tests/integration/` |

**产出**：Worker 可从 S3 拉取数据并写入 BlockStore，形成完整缓存链路。AsyncCacheBlock 后续迭代。

---

### Phase 4: WriteFile 完整支持（P0，CLI 前置）

| 步骤 | 描述 | 文件 |
|------|------|------|
| 4.1 | CreateFileResponse 增加 worker_address | `proto/master.proto` |
| 4.2 | MasterServiceImpl 填充 worker_address | `src/master/master_service_impl.cpp` |
| 4.3 | CreateFileEx 返回 worker_address；WriteFile 在 locations 为空时使用该 worker | `src/client/file_system_client.h/cpp` |
| 4.4 | Worker WriteBlock 成功后向 Master ReportBlockLocation | `src/worker/worker_service_impl.cpp` 或 worker 内部 |
| 4.5 | 测试：CreateFile + WriteFile 新文件 + ReadFile 验证 | `tests/client/` 或 手动 |

**产出**：WriteFile 支持新文件、新 block 的完整写入，CLI write 无限制。

---

### Phase 5: CLI fs 命令组（P2）

| 步骤 | 描述 | 文件 |
|------|------|------|
| 5.1 | 新增 cat、head、tail、touch、write、location、test、cp | `tools/anycache_cli_main.cpp` |
| 5.2 | 统一 ls、mkdir、mv、rm、stat 于同一 help 输出 | 同上 |
| 5.3 | 更新 PrintUsage、docs/usage.md、README | `tools/anycache_cli_main.cpp`、`docs/usage.md`、`README.md` |

**产出**：CLI 具备完整 fs 命令组，与 Alluxio 类似，支持创建、读取、写入等，无限制。

---

## 执行顺序

```
Phase 1 (P3) → Phase 2 (P0) → Phase 3 (P0) → Phase 4 (P0) → Phase 5 (P2)
```

- Phase 1：MountTable 配置基础
- Phase 2：S3 UFS 可用（MinIO/CubeFS）
- Phase 3：CacheBlock 同步实现
- Phase 4：WriteFile 完整支持（CLI 前置）
- Phase 5：CLI fs 命令组

## Risks & Assumptions

- **AWS SDK**：需在目标平台可用。MinIO、CubeFS 兼容 S3 协议，可用于本地测试。
- **测试环境**：建议本地部署 MinIO 或 CubeFS，无需 AWS 凭证。
- **Worker ReportBlockLocation**：WriteBlock 后需上报，否则 ReadFile 无法通过 GetBlockLocations 找到 block；需确认 Worker 是否已有上报逻辑，若无则补充。
