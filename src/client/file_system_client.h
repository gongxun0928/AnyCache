#pragma once

#include "client/channel_pool.h"
#include "client/client_config.h"
#include "common/status.h"
#include "common/types.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "master.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace anycache {

// FileInfo as returned to client
struct ClientFileInfo {
  InodeId inode_id;
  std::string name;
  std::string path;
  bool is_directory;
  uint64_t size;
  uint32_t mode;
  int64_t modification_time_ms;
};

// BlockLocationInfo as returned to client (mirrors the master-side struct)
struct ClientBlockLocation {
  BlockId block_id;
  WorkerId worker_id;
  std::string worker_address;
  TierType tier;
};

// FileSystemClient provides the client-side file operation API via gRPC.
//
// Internally holds a ChannelPool that caches gRPC Channels to workers.
// This means multiple ReadFile/WriteFile calls reuse the same underlying
// HTTP/2 connections, avoiding per-block TCP handshake overhead.
//
// All RPC calls enforce a configurable deadline to prevent IO thread hangs.
class FileSystemClient {
public:
  // Connect to Master via gRPC (default timeouts).
  explicit FileSystemClient(const std::string &master_address);

  // Connect using client config (e.g. ClientConfig::LoadFromFile(path)).
  explicit FileSystemClient(const ClientConfig &config);

  // Connect with a shared ChannelPool and client config.
  FileSystemClient(const std::string &master_address,
                   std::shared_ptr<ChannelPool> channel_pool,
                   const ClientConfig &config);

  ~FileSystemClient();

  // ─── File operations ─────────────────────────────────────
  Status GetFileInfo(const std::string &path, ClientFileInfo *info);
  Status CreateFile(const std::string &path, uint32_t mode = 0644);
  Status CreateFileEx(const std::string &path, uint32_t mode, InodeId *out_id,
                      WorkerId *out_worker_id,
                      std::string *out_worker_address = nullptr);
  Status CompleteFile(InodeId file_id, uint64_t size);
  Status DeleteFile(const std::string &path, bool recursive = false);
  Status RenameFile(const std::string &src, const std::string &dst);
  Status ListStatus(const std::string &path,
                    std::vector<ClientFileInfo> *entries);
  Status Mkdir(const std::string &path, bool recursive = false);
  Status TruncateFile(const std::string &path, uint64_t new_size);

  // ─── Mount operations ────────────────────────────────────
  Status Mount(const std::string &anycache_path, const std::string &ufs_uri);
  Status Unmount(const std::string &anycache_path);
  Status
  GetMountTable(std::vector<std::pair<std::string, std::string>> *mounts);

  // ─── Block operations ────────────────────────────────────
  Status GetBlockLocations(const std::vector<BlockId> &block_ids,
                           std::vector<ClientBlockLocation> *locations);

  // ─── Read/Write convenience ──────────────────────────────
  Status ReadFile(const std::string &path, void *buf, size_t size, off_t offset,
                  size_t *bytes_read);
  Status WriteFile(const std::string &path, const void *buf, size_t size,
                   off_t offset, size_t *bytes_written);

  // ─── Channel pool access ─────────────────────────────────
  std::shared_ptr<ChannelPool> GetChannelPool() const { return channel_pool_; }

private:
  // Apply deadline for Client → Master RPCs.
  void SetMasterDeadline(grpc::ClientContext &ctx) const;
  // Apply deadline for Client → Worker RPCs (block transfers).
  void SetWorkerDeadline(grpc::ClientContext &ctx) const;

  std::shared_ptr<ChannelPool> channel_pool_;
  std::shared_ptr<grpc::Channel> channel_; // Master channel
  std::unique_ptr<proto::MasterService::Stub> stub_;

  // Timeout durations (0 = no deadline)
  std::chrono::milliseconds master_timeout_;
  std::chrono::milliseconds worker_timeout_;
};

} // namespace anycache
