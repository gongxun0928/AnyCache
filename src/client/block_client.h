#pragma once

#include "common/status.h"
#include "common/types.h"

#include <chrono>
#include <memory>
#include <string>

#include "worker.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace anycache {

// BlockClient reads/writes blocks from/to workers via gRPC.
//
// Preferred usage: construct with a shared Channel from ChannelPool
// so that multiple BlockClient instances to the same worker share
// the underlying HTTP/2 connection.
//
// All RPC calls enforce a configurable deadline to prevent IO hangs.
class BlockClient {
public:
  // Construct from a pre-existing Channel (preferred — reuses connection).
  // timeout_ms: per-RPC deadline; 0 = no deadline.
  explicit BlockClient(
      std::shared_ptr<grpc::Channel> channel,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));

  // Construct by address (creates a new Channel — use ChannelPool instead).
  explicit BlockClient(
      const std::string &worker_address,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));

  Status ReadBlock(BlockId id, void *buf, size_t size, off_t offset);
  Status WriteBlock(BlockId id, const void *buf, size_t size, off_t offset);
  Status RemoveBlock(BlockId id);

private:
  void SetDeadline(grpc::ClientContext &ctx) const;

  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<proto::WorkerService::Stub> stub_;
  std::chrono::milliseconds timeout_;
};

} // namespace anycache
