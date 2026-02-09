# AnyCache - 分布式缓存加速系统

AnyCache 是一个 C++20 实现的分布式缓存文件系统，参考 Alluxio/GooseFS 的架构，提供统一命名空间和多级缓存加速能力。

## 架构

```
Client (FUSE / SDK / CLI)
        │  gRPC
        ▼
   Master Cluster (元数据 + 命名空间)
        │  gRPC
        ▼
   Worker Pool (多级缓存: Memory → SSD → HDD)
        │
        ▼
   Under File System (Local FS / S3 / HDFS)
```

所有组件间通信均通过 gRPC，Client 通过连接池（ChannelPool）复用 HTTP/2 连接，所有 RPC 调用均配置可调超时，防止 IO 线程阻塞。

## 核心特性

- **统一命名空间 (MountTable)**: 多种底层存储挂载到统一路径
- **多级缓存 (StorageTier)**: Memory / SSD / HDD 分层，自动晋升/淘汰
- **Page 级缓存 (PageStore)**: 1MB 粒度缓存，优化随机小 IO
- **RocksDB 元数据持久化 (MetaStore)**: Worker 重启后快速恢复缓存索引
- **可插拔淘汰策略**: LRU / LFU
- **gRPC 通信**: 全链路 RPC，支持连接池复用与可配置超时
- **FUSE 挂载**: POSIX 兼容，可用标准工具操作
- **S3 后端**: 兼容 AWS S3 协议的对象存储
- **自动缓存层级管理**: 热数据自动晋升、容量不足自动淘汰
- **Master HA**: 基于 Raft 的 Journal 复制（骨架）
- **监控指标**: Prometheus 兼容的 /metrics HTTP 端点

## 构建

### 依赖

| 依赖 | 类型 | 用途 | 安装 |
|------|------|------|------|
| CMake 3.20+ | 必选 | 构建系统 | - |
| C++20 编译器 | 必选 | GCC 11+ / Clang 14+ / Apple Clang 15+ | - |
| gRPC + Protobuf | 必选 | 分布式 RPC 通信 | macOS: `brew install grpc protobuf`<br>Linux: `apt install libgrpc++-dev protobuf-compiler-grpc` |
| RocksDB | 自动 | 元数据持久化 | 自动通过 FetchContent 下载 |
| libfuse3 | 可选 | FUSE 挂载 | `apt install libfuse3-dev` |
| AWS SDK for C++ | 可选 | S3 后端 | 参考 AWS 文档 |

> **macOS 注意**: 如果 CMake 找不到 Protobuf/gRPC，运行 `brew link protobuf grpc` 使其可被发现。

### 编译

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)    # Linux
make -j$(sysctl -n hw.ncpu)  # macOS

# 运行测试
ctest --output-on-failure

# 运行 benchmark
./cache_benchmark
```

## 使用

```bash
# 启动 Master
./anycache-master [config.yaml]

# 启动 Worker
./anycache-worker [config.yaml]

# CLI 操作 (默认连接 localhost:19999)
./anycache-cli ls /
./anycache-cli mkdir /data
./anycache-cli stat /data
./anycache-cli rm /data
./anycache-cli mv /data /data2

# 挂载/卸载 UFS
./anycache-cli mount /data s3://my-bucket/prefix
./anycache-cli unmount /data
./anycache-cli mountTable

# 指定 Master 地址
./anycache-cli --master 10.0.0.1:19999 ls /

# FUSE 挂载 (需要 libfuse3)
./anycache-fuse /mnt/anycache -f
```

## 配置

参见 `config/example.yaml`，主要配置段：

| 配置段 | 说明 |
|--------|------|
| `master` | Master 监听地址、端口、Journal 目录、心跳超时、metrics 端口 |
| `worker` | Worker 监听地址、端口、连接的 Master 地址、存储层配置、metrics 端口 |
| `fuse` | FUSE 挂载点、Master 地址、direct_io 开关 |
| `rpc` | RPC 超时配置（Client→Master / Client→Worker / Worker→Master） |
| `s3` | S3 后端连接信息 |

### RPC 超时配置

```yaml
rpc:
  master_rpc_timeout_ms: 10000    # Client → Master 超时 (元数据操作, 默认 10s)
  worker_rpc_timeout_ms: 30000    # Client → Worker 超时 (块读写, 默认 30s)
  internal_rpc_timeout_ms: 10000  # Worker → Master 超时 (注册/心跳, 默认 10s)
```

设为 `0` 表示不设超时（不推荐在生产环境使用）。

## 项目结构

```
anycache/
├── proto/           # gRPC Protobuf 定义 (master.proto, worker.proto, common.proto)
├── src/
│   ├── common/      # 公共: Status, Config, Logging, Metrics, Types, ProtoUtils
│   ├── master/      # Master: InodeTree, BlockMaster, WorkerManager, Journal,
│   │                #         MasterServer, MasterServiceImpl
│   ├── worker/      # Worker: StorageTier, BlockStore, PageStore, CacheManager,
│   │                #         MetaStore, WorkerServer, WorkerServiceImpl, MasterClient
│   ├── client/      # Client: FileSystemClient, BlockClient, CacheClient, ChannelPool
│   ├── fuse/        # FUSE: fuse_operations, fuse_main
│   └── ufs/         # UFS: LocalUFS, S3UFS, Factory
├── config/          # 配置示例
├── tests/           # Google Test 单元测试
├── benchmark/       # Google Benchmark 性能测试
└── tools/           # 可执行入口: master, worker, cli
```
