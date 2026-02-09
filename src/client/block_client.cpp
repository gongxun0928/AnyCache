#include "client/block_client.h"
#include "client/client_proto_utils.h"

namespace anycache {

BlockClient::BlockClient(std::shared_ptr<grpc::Channel> channel,
                         std::chrono::milliseconds timeout)
    : channel_(std::move(channel)),
      stub_(proto::WorkerService::NewStub(channel_)), timeout_(timeout) {}

BlockClient::BlockClient(const std::string &worker_address,
                         std::chrono::milliseconds timeout)
    : channel_(grpc::CreateChannel(worker_address,
                                   grpc::InsecureChannelCredentials())),
      stub_(proto::WorkerService::NewStub(channel_)), timeout_(timeout) {}

void BlockClient::SetDeadline(grpc::ClientContext &ctx) const {
  if (timeout_.count() > 0) {
    ctx.set_deadline(std::chrono::system_clock::now() + timeout_);
  }
}

Status BlockClient::ReadBlock(BlockId id, void *buf, size_t size,
                              off_t offset) {
  proto::ReadBlockRequest req;
  req.set_block_id(id);
  req.set_offset(static_cast<uint64_t>(offset));
  req.set_length(size);

  proto::ReadBlockResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);

  auto grpc_status = stub_->ReadBlock(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  RETURN_IF_ERROR(FromProtoStatus(resp.status()));

  size_t copy_size = std::min(size, resp.data().size());
  std::memcpy(buf, resp.data().data(), copy_size);
  return Status::OK();
}

Status BlockClient::WriteBlock(BlockId id, const void *buf, size_t size,
                               off_t offset) {
  proto::WriteBlockRequest req;
  req.set_block_id(id);
  req.set_offset(static_cast<uint64_t>(offset));
  req.set_data(buf, size);

  proto::WriteBlockResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);

  auto grpc_status = stub_->WriteBlock(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  return FromProtoStatus(resp.status());
}

Status BlockClient::RemoveBlock(BlockId id) {
  proto::RemoveBlockRequest req;
  req.set_block_id(id);

  proto::RemoveBlockResponse resp;
  grpc::ClientContext ctx;
  SetDeadline(ctx);

  auto grpc_status = stub_->RemoveBlock(&ctx, req, &resp);
  if (!grpc_status.ok())
    return Status::Unavailable(grpc_status.error_message());
  return FromProtoStatus(resp.status());
}

} // namespace anycache
