#pragma once

#include "common/status.h"
#include "common/types.h"
#include "master/inode_tree.h"

#include "common.pb.h"
#include "master.pb.h"
#include "worker.pb.h"

namespace anycache {

// Forward declaration
struct ClientFileInfo;

// ─── Status conversion ───────────────────────────────────────

inline proto::StatusCode ToProtoStatusCode(StatusCode code) {
  switch (code) {
  case StatusCode::kOk:
    return proto::OK;
  case StatusCode::kNotFound:
    return proto::NOT_FOUND;
  case StatusCode::kAlreadyExists:
    return proto::ALREADY_EXISTS;
  case StatusCode::kInvalidArgument:
    return proto::INVALID_ARGUMENT;
  case StatusCode::kIOError:
    return proto::IO_ERROR;
  case StatusCode::kPermissionDenied:
    return proto::PERMISSION_DENIED;
  case StatusCode::kNotImplemented:
    return proto::NOT_IMPLEMENTED;
  case StatusCode::kResourceExhausted:
    return proto::RESOURCE_EXHAUSTED;
  case StatusCode::kUnavailable:
    return proto::UNAVAILABLE;
  case StatusCode::kInternal:
    return proto::INTERNAL;
  default:
    return proto::INTERNAL;
  }
}

inline proto::RpcStatus ToProtoStatus(const Status &s) {
  proto::RpcStatus ps;
  ps.set_code(ToProtoStatusCode(s.code()));
  if (!s.ok()) {
    ps.set_message(s.message());
  }
  return ps;
}

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

// ─── TierType conversion ─────────────────────────────────────

inline proto::TierType ToProtoTier(TierType t) {
  switch (t) {
  case TierType::kMemory:
    return proto::MEMORY;
  case TierType::kSSD:
    return proto::SSD;
  case TierType::kHDD:
    return proto::HDD;
  }
  return proto::MEMORY;
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

// ─── Inode / FileInfo conversion ─────────────────────────────

inline proto::FileInfo InodeToProto(const Inode &inode) {
  proto::FileInfo fi;
  fi.set_inode_id(inode.id);
  fi.set_name(inode.name);
  fi.set_is_directory(inode.is_directory);
  fi.set_size(inode.size);
  fi.set_mode(inode.mode);
  fi.set_owner(inode.owner);
  fi.set_group(inode.group);
  fi.set_creation_time_ms(inode.creation_time_ms);
  fi.set_modification_time_ms(inode.modification_time_ms);
  fi.set_parent_id(inode.parent_id);
  fi.set_block_size(inode.block_size);
  fi.set_is_complete(inode.is_complete);
  return fi;
}

inline Inode ProtoToInode(const proto::FileInfo &fi) {
  Inode inode;
  inode.id = fi.inode_id();
  inode.parent_id = fi.parent_id();
  inode.name = fi.name();
  inode.is_directory = fi.is_directory();
  inode.size = fi.size();
  inode.mode = fi.mode();
  inode.owner = fi.owner();
  inode.group = fi.group();
  inode.creation_time_ms = fi.creation_time_ms();
  inode.modification_time_ms = fi.modification_time_ms();
  inode.block_size = fi.block_size() > 0 ? fi.block_size() : kDefaultBlockSize;
  inode.is_complete = fi.is_complete();
  return inode;
}

// ─── BlockLocationInfo conversion ────────────────────────────

inline proto::BlockLocation BlockLocationToProto(const BlockLocationInfo &loc) {
  proto::BlockLocation bl;
  bl.set_block_id(loc.block_id);
  bl.set_worker_id(loc.worker_id);
  bl.set_worker_address(loc.worker_address);
  bl.set_tier(ToProtoTier(loc.tier));
  return bl;
}

inline BlockLocationInfo ProtoToBlockLocation(const proto::BlockLocation &bl) {
  BlockLocationInfo loc;
  loc.block_id = bl.block_id();
  loc.worker_id = bl.worker_id();
  loc.worker_address = bl.worker_address();
  loc.tier = FromProtoTier(bl.tier());
  return loc;
}

} // namespace anycache
