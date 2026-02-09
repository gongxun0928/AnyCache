# TODO-11: 修复 WriteFile 对新文件的完整写入支持

> 状态: 已完成
> 优先级: P0（CLI fs 命令组前置依赖）
> 依赖: 无

## 目标

使 `FileSystemClient::WriteFile` 支持**新创建文件**和**新 block** 的完整写入，消除「no worker available for block」限制，为 CLI 的 fs 命令组（cat、write、touch 等）提供正确性基础。

## 现状

- `CreateFile` 返回 `(file_id, worker_id)`，但 Client 需 `worker_address` 才能向 Worker 写 block
- 新 block 尚未被 Worker 上报，`GetBlockLocations` 返回空
- Client 在 locations 为空时直接返回错误，无法使用 CreateFile 分配的 worker

## 实现任务

1. **CreateFileResponse 增加 worker_address**
   - `proto/master.proto`：`CreateFileResponse` 增加 `string worker_address = 4`
   - `MasterServiceImpl`：从 `WorkerManager::GetWorker(worker_id)` 取 address，写入 response
   - 若 worker_id 无效或 GetWorker 失败，worker_address 为空

2. **CreateFileEx 返回 worker_address**
   - `FileSystemClient::CreateFileEx`：从 response 取 worker_address，通过输出参数或扩展返回
   - 或新增 `CreateFileEx(path, mode, &file_id, &worker_id, &worker_address)`

3. **WriteFile 使用分配的 worker**
   - 在 `WriteFile` 中，当 `GetBlockLocations` 返回空时：
     - 若本次 write 来自刚创建的 file（通过 CreateFileEx 拿到 worker_address），使用该 address
     - 需在 WriteFile 中缓存 CreateFileEx 返回的 worker_address（或每次按 block 查不到时，用 last known assigned worker）
   - 实现：WriteFile 在「file 不存在先 CreateFileEx」时，保存返回的 worker_address；后续对「locations 为空」的 block，使用该 worker_address 写入
   - Worker 写入成功后应 `ReportBlockLocation`，以便后续 read 能通过 GetBlockLocations 找到（需确认 Worker WriteBlock 后是否上报）

4. **Worker WriteBlock 后上报 BlockLocation**
   - 若 Worker 在 WriteBlock 成功后未向 Master 上报，需在 `WorkerServiceImpl::WriteBlock` 或 Worker 启动/心跳逻辑中补充 `ReportBlockLocation` 调用
   - 确保写完后，GetBlockLocations 能返回该 block 的位置

5. **测试**
   - 单元测试：CreateFile + WriteFile 新文件，ReadFile 能正确读出
   - 集成测试：CLI touch + write + cat 全流程

## 参考

- `src/client/file_system_client.cpp`：WriteFile 当前逻辑
- `src/master/worker_manager.h`：GetWorker(worker_id) -> WorkerState.address
- `proto/master.proto`：CreateFileResponse
