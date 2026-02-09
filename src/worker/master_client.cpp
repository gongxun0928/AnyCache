#include "worker/master_client.h"
#include "common/logging.h"
#include "common/proto_utils.h"

namespace anycache {

MasterClient::MasterClient(const std::string &master_address)
    : MasterClient(master_address, std::chrono::milliseconds(10000)) {}

MasterClient::MasterClient(const std::string &master_address,
                           std::chrono::milliseconds timeout)
    : channel_(grpc::CreateChannel(master_address,
                                   grpc::InsecureChannelCredentials())),
      stub_(proto::MasterService::NewStub(channel_)), timeout_(timeout) {
  LOG_INFO("MasterClient connecting to {} (timeout={}ms)", master_address,
           timeout.count());
}

void MasterClient::SetDeadline(grpc::ClientContext &ctx) const {
  if (timeout_.count() > 0) {
    ctx.set_deadline(std::chrono::system_clock::now() + timeout_);
  }
}

Status MasterClient::RegisterWorker(const std::string &self_address,
                                    uint64_t capacity, uint64_t used,
                                    WorkerId *out_id) {
  proto::RegisterWorkerRequest req;
  req.set_address(self_address);
  req.set_capacity_bytes(capacity);
  req.set_used_bytes(used);

  proto::RegisterWorkerResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);

  auto grpc_status = stub_->RegisterWorker(&ctx, req, &resp);
  if (!grpc_status.ok()) {
    return Status::Unavailable("RegisterWorker RPC failed: " +
                               grpc_status.error_message());
  }

  RETURN_IF_ERROR(FromProtoStatus(resp.status()));
  *out_id = resp.worker_id();
  return Status::OK();
}

Status MasterClient::Heartbeat(WorkerId id, uint64_t capacity, uint64_t used) {
  proto::WorkerHeartbeatRequest req;
  req.set_worker_id(id);
  req.set_capacity_bytes(capacity);
  req.set_used_bytes(used);

  proto::WorkerHeartbeatResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);

  auto grpc_status = stub_->WorkerHeartbeat(&ctx, req, &resp);
  if (!grpc_status.ok()) {
    return Status::Unavailable("WorkerHeartbeat RPC failed: " +
                               grpc_status.error_message());
  }

  return FromProtoStatus(resp.status());
}

Status MasterClient::ReportBlockLocation(BlockId block_id, WorkerId worker_id,
                                         const std::string &worker_address,
                                         TierType tier) {
  proto::ReportBlockLocationRequest req;
  req.set_worker_id(worker_id);
  auto *bl = req.add_blocks();
  bl->set_block_id(block_id);
  bl->set_worker_id(worker_id);
  bl->set_worker_address(worker_address);
  bl->set_tier(ToProtoTier(tier));

  proto::ReportBlockLocationResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);

  auto grpc_status = stub_->ReportBlockLocation(&ctx, req, &resp);
  if (!grpc_status.ok()) {
    return Status::Unavailable("ReportBlockLocation RPC failed: " +
                               grpc_status.error_message());
  }

  return FromProtoStatus(resp.status());
}

} // namespace anycache
