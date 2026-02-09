#pragma once

#include "common/status.h"
#include "common/types.h"

#include "common.pb.h"

namespace anycache {

// Client-only proto conversions. Does not depend on master/worker.
inline Status FromProtoStatus(const proto::RpcStatus &ps) {
  if (ps.code() == proto::OK) {
    return Status::OK();
  }
  switch (ps.code()) {
  case proto::NOT_FOUND:
    return Status::NotFound(ps.message());
  case proto::ALREADY_EXISTS:
    return Status::AlreadyExists(ps.message());
  case proto::INVALID_ARGUMENT:
    return Status::InvalidArgument(ps.message());
  case proto::IO_ERROR:
    return Status::IOError(ps.message());
  case proto::PERMISSION_DENIED:
    return Status::PermissionDenied(ps.message());
  case proto::NOT_IMPLEMENTED:
    return Status::NotImplemented(ps.message());
  case proto::RESOURCE_EXHAUSTED:
    return Status::ResourceExhausted(ps.message());
  case proto::UNAVAILABLE:
    return Status::Unavailable(ps.message());
  case proto::INTERNAL:
    return Status::Internal(ps.message());
  default:
    return Status::Internal(ps.message());
  }
}

inline TierType FromProtoTier(proto::TierType t) {
  switch (t) {
  case proto::MEMORY:
    return TierType::kMemory;
  case proto::SSD:
    return TierType::kSSD;
  case proto::HDD:
    return TierType::kHDD;
  default:
    return TierType::kMemory;
  }
}

} // namespace anycache
