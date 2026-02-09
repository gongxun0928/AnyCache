#pragma once

#include "master/file_system_master.h"
#include "master/mount_table.h"

#include "master.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace anycache {

// MasterServiceImpl bridges gRPC requests to the FileSystemMaster.
class MasterServiceImpl final : public proto::MasterService::Service {
public:
  MasterServiceImpl(FileSystemMaster *master, MountTable *mount_table);

  // ─── File operations ─────────────────────────────────────
  grpc::Status GetFileInfo(grpc::ServerContext *ctx,
                           const proto::GetFileInfoRequest *req,
                           proto::GetFileInfoResponse *resp) override;

  grpc::Status CreateFile(grpc::ServerContext *ctx,
                          const proto::CreateFileRequest *req,
                          proto::CreateFileResponse *resp) override;

  grpc::Status CompleteFile(grpc::ServerContext *ctx,
                            const proto::CompleteFileRequest *req,
                            proto::CompleteFileResponse *resp) override;

  grpc::Status DeleteFile(grpc::ServerContext *ctx,
                          const proto::DeleteFileRequest *req,
                          proto::DeleteFileResponse *resp) override;

  grpc::Status RenameFile(grpc::ServerContext *ctx,
                          const proto::RenameFileRequest *req,
                          proto::RenameFileResponse *resp) override;

  grpc::Status ListStatus(grpc::ServerContext *ctx,
                          const proto::ListStatusRequest *req,
                          proto::ListStatusResponse *resp) override;

  grpc::Status Mkdir(grpc::ServerContext *ctx, const proto::MkdirRequest *req,
                     proto::MkdirResponse *resp) override;

  grpc::Status TruncateFile(grpc::ServerContext *ctx,
                            const proto::TruncateFileRequest *req,
                            proto::TruncateFileResponse *resp) override;

  // ─── Block operations ────────────────────────────────────
  grpc::Status
  GetBlockLocations(grpc::ServerContext *ctx,
                    const proto::GetBlockLocationsRequest *req,
                    proto::GetBlockLocationsResponse *resp) override;

  grpc::Status
  ReportBlockLocation(grpc::ServerContext *ctx,
                      const proto::ReportBlockLocationRequest *req,
                      proto::ReportBlockLocationResponse *resp) override;

  // ─── Worker management ───────────────────────────────────
  grpc::Status RegisterWorker(grpc::ServerContext *ctx,
                              const proto::RegisterWorkerRequest *req,
                              proto::RegisterWorkerResponse *resp) override;

  grpc::Status WorkerHeartbeat(grpc::ServerContext *ctx,
                               const proto::WorkerHeartbeatRequest *req,
                               proto::WorkerHeartbeatResponse *resp) override;

  // ─── Mount operations ────────────────────────────────────
  grpc::Status Mount(grpc::ServerContext *ctx, const proto::MountRequest *req,
                     proto::MountResponse *resp) override;

  grpc::Status Unmount(grpc::ServerContext *ctx,
                       const proto::UnmountRequest *req,
                       proto::UnmountResponse *resp) override;

  grpc::Status GetMountTable(grpc::ServerContext *ctx,
                             const proto::GetMountTableRequest *req,
                             proto::GetMountTableResponse *resp) override;

private:
  FileSystemMaster *master_;
  MountTable *mount_table_;
};

} // namespace anycache
