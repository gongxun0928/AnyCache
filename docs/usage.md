# AnyCache Client 使用说明

本文档面向测试同学，说明如何通过 AnyCache 提供的 **Client 库** 编写测试用例，以及当前读写能力的实现状态。

---

## 一、当前读写实现状态

| 能力 | 状态 | 说明 |
|------|------|------|
| **读（Read）** | ✅ 已实现 | `ReadFile(path, buf, size, offset, bytes_read)`：通过 Master 查文件元数据与 block 位置，再向 Worker 拉取数据。适用于**已有 block 位置登记**的文件。 |
| **写（Write）** | ⚠ 部分可用 | `WriteFile(path, buf, size, offset, bytes_written)`：对**已存在且 Master 中已有 block 位置**的文件可写；对**新创建文件或新 block** 会因无法解析「写入目标 Worker 地址」而返回错误（`no worker available for block`）。 |
| **元数据与目录** | ✅ 已实现 | `GetFileInfo`、`ListStatus`、`Mkdir`、`CreateFile`、`CompleteFile`、`DeleteFile`、`RenameFile`、`GetBlockLocations` 等均已实现。 |

**测试建议**：

- **读相关用例**：可直接基于 `FileSystemClient::ReadFile` 与 `GetFileInfo` / `GetBlockLocations` 编写。
- **写相关用例**：当前优先用 **CreateFile + 低层 BlockClient 写 + CompleteFile** 的流程（需测试环境能拿到 Worker 地址），或依赖 CLI/FUSE 等已集成写路径的入口做集成测试；待服务端返回「创建文件时分配的 Worker 地址」并补齐 Client 对新 block 的写逻辑后，再统一使用 `WriteFile`。

---

## 二、Client 库与依赖

- **库名**：`anycache_client`（CMake 目标，可独立打包）。
- **主要头文件**：`client/file_system_client.h`、`client/block_client.h`、`client/client_config.h`、`client/channel_pool.h`。
- **依赖**：`anycache_common`、`anycache_proto`、yaml-cpp（配置从文件加载）；**不依赖** Master/Worker 实现。可选本地页缓存组件为 `anycache_client_cache`（依赖 worker，见 `client/cache/cache_client.h`）。

测试可执行文件需要链接 `anycache_client`（及 CMake 已声明的其依赖），并包含 `src` 为头文件搜索路径（与 `tools/anycache_cli_main.cpp` 一致）。

---

## 三、环境准备

1. **构建**  
   在项目根目录执行：

   ```bash
   mkdir -p build && cd build
   cmake .. && cmake --build .
   ```

2. **运行服务（测试前）**  
   - 启动 Master：`./anycache-master ...`（或按项目配置指定地址，默认如 `localhost:19999`）。  
   - 启动至少一个 Worker：`./anycache-worker ...`（并配置 `master_address` 指向当前 Master）。  
   - 确保 Master 与 Worker 已成功注册（可查看日志或通过 CLI 做一次 `ls /` 等操作验证）。

3. **Master 地址**  
   Client 仅需连接 Master 的 gRPC 地址（例如 `localhost:19999`）；与 Worker 的通信由 Client 内部通过 Master 返回的 `GetBlockLocations` 中的 `worker_address` 自动建立（使用内置 `ChannelPool` 复用连接）。

---

## 四、连接与构造方式

```cpp
#include "client/file_system_client.h"
#include "client/client_config.h"

// 仅指定 Master 地址（使用默认超时与内部 ChannelPool）
anycache::FileSystemClient client("localhost:19999");

// 从指定配置文件加载（推荐：便于部署与测试）
anycache::ClientConfig config = anycache::ClientConfig::LoadFromFile("/etc/anycache/client.yaml");
anycache::FileSystemClient client(config);
// 或一行：anycache::FileSystemClient client(anycache::ClientConfig::LoadFromFile("/path/to/config.yaml"));

// 使用已有 ChannelPool 与配置
auto pool = std::make_shared<anycache::ChannelPool>();
anycache::FileSystemClient client(config.master_address, pool, config);
```

配置文件示例（YAML，可与服务端共用同一文件，只读 `client` 或 `rpc` 段）：

```yaml
client:
  master_address: "localhost:19999"
  master_rpc_timeout_ms: 10000   # Client -> Master
  worker_rpc_timeout_ms: 30000    # Client -> Worker

# 若无 client 段，会回退到 fuse.master_address 或 master.host:port
```

所有 RPC 都会按配置设置 deadline，避免无限阻塞。

---

## 五、常用 API 与用法

### 5.1 文件与目录（推荐用于测试）

- **GetFileInfo(path, &info)**  
  获取文件/目录元数据；`ClientFileInfo` 包含 `inode_id`、`name`、`path`、`is_directory`、`size`、`mode`、`modification_time_ms` 等。

- **ListStatus(path, &entries)**  
  列出目录下子项，`entries` 为 `std::vector<ClientFileInfo>`。

- **Mkdir(path, recursive)**  
  创建目录；`recursive == true` 时等价于 `mkdir -p`。

- **CreateFile(path, mode)**  
  在命名空间中创建文件；成功后可拿到 inode，但当前**不返回 Worker 地址**，因此直接对**新文件**调用 `WriteFile` 会失败。

- **CreateFileEx(path, mode, &out_inode_id, &out_worker_id)**  
  创建文件并返回 `file_id`（inode_id）与 Master 分配的 `worker_id`（用于写）；当前 Client 未提供按 `worker_id` 解析 Worker 地址的接口，故写新 block 需后续扩展。

- **CompleteFile(file_id, size)**  
  在写完成后通知 Master 文件大小（用于正确解释 block 布局与 size）。

- **DeleteFile(path, recursive)**  
  删除文件或目录。

- **RenameFile(src_path, dst_path)**  
  重命名/移动。

### 5.2 块级（适合进阶或写路径测试）

- **GetBlockLocations(block_ids, &locations)**  
  根据 block_id 列表查询 Master，得到 `ClientBlockLocation`（含 `worker_address`、`worker_id`、`tier`）。  
  `block_id` 可由 `anycache::MakeBlockId(inode_id, block_index)` 得到，块大小默认 `kDefaultBlockSize`（如 64MB）。

- **BlockClient**  
  面向单 Worker 的块读写：`ReadBlock(block_id, buf, size, offset)`、`WriteBlock(block_id, buf, size, offset)`。  
  建议通过 `FileSystemClient::GetChannelPool()->GetChannel(worker_address)` 拿到 Channel 再构造 `BlockClient`，以复用连接（与 `ReadFile`/`WriteFile` 内部一致）。

### 5.3 便捷读写（当前建议用法）

- **ReadFile(path, buf, size, offset, &bytes_read)**  
  从 path 的 offset 起最多读 size 字节到 buf，实际读到的长度写入 `bytes_read`。  
  内部会：GetFileInfo → 按 block 切分 → GetBlockLocations → 对每个 block 用 BlockClient 读。  
  **适用**：文件已存在且 block 已在 Master 登记（例如由 Worker 通过 ReportBlockLocation 上报，或由其他路径写入并上报）。

- **WriteFile(path, buf, size, offset, &bytes_written)**  
  向 path 的 offset 起写入最多 size 字节。  
  若 path 不存在会先 CreateFile；然后按 block 切分，对每个 block 调用 GetBlockLocations；若**该 block 尚无位置**则当前实现会返回 `no worker available for block`，因此**对新文件或新 block 不可用**。  
  测试写逻辑时，可改用：**CreateFileEx + BlockClient 写 + CompleteFile**（需在测试环境中能获得 Worker 地址，或等后续扩展 Client 使用 CreateFile 返回的 worker 信息）。

---

## 六、测试用例撰写流程（推荐）

1. **链接与头文件**  
   - 测试目标 `target_link_libraries(your_test_target PRIVATE anycache_client anycache_common)`（若 CMake 已集中管理依赖，按项目 test 规范即可）。  
   - 包含 `#include "client/file_system_client.h"`，必要时 `#include "client/block_client.h"`。

2. **启动与连接**  
   - 在用例或 fixture 中假定 Master 已启动（或通过 test 环境变量读取 `ANYCACHE_MASTER_ADDRESS`，默认 `localhost:19999`）。  
   - 构造 `anycache::FileSystemClient client(master_address)`（或带 `RpcConfig`/`ChannelPool` 的版本）。

3. **读与元数据用例**  
   - 先 Mkdir / 使用已有目录，再 GetFileInfo、ListStatus。  
   - 对已有数据的文件调用 ReadFile，校验 `bytes_read` 与 buf 内容。  
   - 可选：GetBlockLocations + BlockClient.ReadBlock 做块级读校验。

4. **写相关用例（当前限制下）**  
   - 使用 CreateFile + CompleteFile 做「创建空文件并声明大小」的元数据测试。  
   - 若需真实写数据：在测试环境中已知 Worker 地址时，可用 CreateFileEx → 用 BlockClient 向该 Worker 写 block → CompleteFile；或依赖 CLI/FUSE 写入后再用 ReadFile 做读校验。

5. **错误与超时**  
   - 所有接口返回 `anycache::Status`，用 `s.ok()`、`s.ToString()` 判断；可针对 `NotFound`、`Unavailable` 等写断言。  
   - 超时由构造 Client 时的 `RpcConfig` 控制，测试可适当缩短以便快速失败。

---

## 七、示例代码片段

```cpp
#include "client/file_system_client.h"
#include "common/logging.h"
#include <cstring>
#include <vector>

void TestReadAndMetadata() {
  anycache::FileSystemClient client("localhost:19999");

  // 列出根目录
  std::vector<anycache::ClientFileInfo> entries;
  auto s = client.ListStatus("/", &entries);
  if (!s.ok()) {
    // 处理错误，例如 Master 未启动
    return;
  }

  // 获取某文件信息
  anycache::ClientFileInfo info;
  s = client.GetFileInfo("/path/to/file", &info);
  if (!s.ok()) return;
  // 使用 info.size, info.inode_id 等

  // 读文件（文件需已存在且 block 已登记）
  std::vector<char> buf(4096);
  size_t bytes_read = 0;
  s = client.ReadFile("/path/to/file", buf.data(), buf.size(), 0, &bytes_read);
  if (s.ok()) {
    // 校验 buf[0..bytes_read)
  }
}

void TestCreateAndComplete() {
  anycache::FileSystemClient client("localhost:19999");

  anycache::InodeId file_id;
  anycache::WorkerId worker_id;
  auto s = client.CreateFileEx("/test/newfile", 0644, &file_id, &worker_id);
  if (!s.ok()) return;

  // 当前未提供 worker_id -> address，此处仅做 CompleteFile 示例
  s = client.CompleteFile(file_id, 0);
  if (!s.ok()) return;
}
```

---

## 八、小结

- **读**：已实现，测试同学可直接用 `FileSystemClient::ReadFile` 与 `GetFileInfo`/`ListStatus`/`GetBlockLocations` 编写读与元数据测试。  
- **写**：当前 `WriteFile` 仅对「已有 block 位置」的文件可用；对新文件/新 block 需使用 CreateFileEx + BlockClient + CompleteFile 或 CLI/FUSE，并注意当前缺少「按 worker_id 解析 Worker 地址」的 Client 能力。  
- **全流程**：准备 Master/Worker 环境 → 构建并链接 `anycache_client` → 构造 `FileSystemClient` → 按上述 API 与约定编写用例，并处理 `Status` 与超时即可。

若后续服务端在 CreateFile 响应中返回 Worker 地址、且 Client 对新 block 使用该地址写入并配合 Worker 上报 block 位置，则 `WriteFile` 将支持完整的「新文件写入」流程，届时测试可统一改为使用 `WriteFile`。
