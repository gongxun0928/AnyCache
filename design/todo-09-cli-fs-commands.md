# TODO-09: CLI fs 命令组（类似 Alluxio）

> 状态: 已完成
> 优先级: P2
> 依赖: TODO-11（WriteFile 对新文件完整支持）

## 目标

实现与 Alluxio `bin/alluxio fs` 类似的 CLI 命令组，提供完整的文件系统操作能力，支持创建、读取、写入等，无功能限制。

## 命令列表（参考 Alluxio）

| 命令 | 说明 | 对应 Client API |
|------|------|-----------------|
| `cat` | 输出文件内容到 stdout | ReadFile |
| `cp` | 复制文件或目录（src → dst，可跨路径） | GetFileInfo, ReadFile, CreateFile, WriteFile, ListStatus |
| `head` | 输出文件前 N 字节 | ReadFile(offset=0, length=N) |
| `location` | 显示文件 block 所在 Worker 及缓存比例 | GetFileInfo, GetBlockLocations |
| `ls` | 列出目录内容 | ListStatus（已有） |
| `mkdir` | 创建目录 | Mkdir（已有） |
| `mv` | 重命名/移动 | RenameFile（已有） |
| `rm` | 删除 | DeleteFile（已有） |
| `stat` | 显示文件/目录元信息 | GetFileInfo（已有） |
| `tail` | 输出文件后 N 字节 | GetFileInfo(size), ReadFile(offset=size-n, length=n) |
| `touch` | 创建 0 字节文件 | CreateFile + CompleteFile |
| `test` | 测试路径属性（-e 存在、-d 目录、-f 文件等） | GetFileInfo |
| `write` | 写入文件（从 stdin 或 --input 文件） | WriteFile |

可选后续：`file-metadata`（export/check/download）等高级操作。

## 实现任务

1. **已有命令**：ls、mkdir、mv、rm、stat 保持并统一到 `fs` 子命令结构（若采用 `anycache-cli fs ls` 则需重构；或保持 `anycache-cli ls` 扁平结构，与 Alluxio 一致）

2. **新增命令**
   - `cat <path>`：读全文件到 stdout
   - `head <path> [-n N]`：前 N 字节，默认 10 行或 1KB（可约定）
   - `tail <path> [-n N]`：后 N 字节
   - `touch <path>`：创建空文件
   - `write <path> [--input FILE]`：从 stdin 或文件写入
   - `cp <src> <dst>`：复制
   - `location <path>`：输出 block 位置
   - `test [-e|-d|-f] <path>`：测试路径属性，退出码 0/1

3. **CLI 结构**
   - 参考 Alluxio：`anycache-cli fs <command> [args]` 或保持 `anycache-cli <command>` 扁平
   - 若扁平：`anycache-cli cat /path`、`anycache-cli touch /path` 等

4. **文档**
   - 更新 `docs/usage.md`、`README.md` 的 CLI 用法
   - `anycache-cli help` 或 `anycache-cli fs` 列出所有命令

## 约束

- **write 必须支持完整创建与写入**：新文件、任意 offset、任意 size，无「仅对已有 block 可写」的限制。依赖 TODO-11 修复 WriteFile。

## 参考

- Alluxio fs 命令：https://docs.alluxio.io/
- `tools/anycache_cli_main.cpp`：现有实现
- `src/client/file_system_client.h`：Client API
