#pragma once

#include "common/config.h"
#include "common/status.h"
#include "common/types.h"

#include <chrono>
#include <memory>
#include <string>

#include "master.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace anycache {

// MasterClient is used by Worker to communicate with the Master.
// It handles worker registration, heartbeats, and block location reports.
//
// All RPC calls enforce a configurable deadline to prevent hangs.
class MasterClient {
public:
  // Construct with default internal timeout (10 s).
  explicit MasterClient(const std::string &master_address);

  // Construct with explicit timeout.
  MasterClient(const std::string &master_address,
               std::chrono::milliseconds timeout);

  // Register this worker with the master
  Status RegisterWorker(const std::string &self_address, uint64_t capacity,
                        uint64_t used, WorkerId *out_id);

  // Send a heartbeat to the master
  Status Heartbeat(WorkerId id, uint64_t capacity, uint64_t used);

  // Report block locations to the master
  Status ReportBlockLocation(BlockId block_id, WorkerId worker_id,
                             const std::string &worker_address, TierType tier);

private:
  void SetDeadline(grpc::ClientContext &ctx) const;

  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<proto::MasterService::Stub> stub_;
  std::chrono::milliseconds timeout_;
};

} // namespace anycache
