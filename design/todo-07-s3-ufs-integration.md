# TODO-07: S3 UFS 接入与完整实现

> 状态: 部分完成（UfsFactory 已接入；S3UnderFileSystem 完整实现需 AWS SDK，安装后可编译）
> 优先级: P0
> 依赖: 无（建议与 TODO-10 配合，MountTable 需完整 Config 才能挂载 S3）

## 目标

实现 S3 后端支持，使 AnyCache 可挂载类 S3 存储（AWS S3、MinIO 等），用于完整测试系统的正确性与性能。

## 现状

- `S3UnderFileSystem` 存在（`src/ufs/s3_ufs.cpp`），但实现为 TODO 占位，AWS SDK 未接入
- `UfsFactory::CreateByScheme` 对 `s3` scheme 返回 `nullptr`（实现被注释）
- `config/example.yaml` 已有 `s3` 配置段

## 实现任务

1. **UfsFactory 接入 S3**
   - 在 `src/ufs/ufs_factory.cpp` 中取消注释并正确调用 `S3UnderFileSystem`
   - 条件编译：`#ifdef ANYCACHE_HAS_S3`

2. **S3UnderFileSystem 完整实现**
   - 初始化 AWS SDK 与 S3Client（使用 `S3Config`）
   - 实现 `Open`、`Read`、`Write`、`Close`、`Create`、`Delete`、`ListDir`、`GetFileInfo`、`Mkdir`、`Exists`、`Rename`
   - 支持 endpoint 覆盖（MinIO 等兼容实现）
   - 支持 `use_path_style`、`region`、`access_key`/`secret_key`（或环境变量）

3. **测试**
   - 单元测试：Mock S3 或使用 MinIO / CubeFS 本地部署
   - 集成测试：挂载 S3（MinIO 或 CubeFS），读写验证

## 参考

- `src/ufs/local_ufs.cpp`：Local UFS 实现参考
- `src/ufs/ufs.h`：UFS 接口定义
- AWS SDK C++ 文档
