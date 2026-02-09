#include "common/config.h"
#include "common/logging.h"
#include "fuse/fuse_operations.h"

#ifdef ANYCACHE_HAS_FUSE
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#endif

#include <iostream>

int main(int argc, char *argv[]) {
  anycache::Logger::Init("fuse");

#ifdef ANYCACHE_HAS_FUSE
  // Parse our config first
  std::string config_path;
  // Simple arg parsing: --config <path>
  std::vector<char *> fuse_argv;
  for (int i = 0; i < argc; ++i) {
    if (std::string(argv[i]) == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else {
      fuse_argv.push_back(argv[i]);
    }
  }

  anycache::Config cfg;
  if (!config_path.empty()) {
    cfg = anycache::Config::LoadFromFile(config_path);
  } else {
    cfg = anycache::Config::Default();
  }

  anycache::InitFuseContext(cfg);

  // Set up FUSE operations
  struct fuse_operations ops = {};
  ops.init = anycache::fuse_ops::Init;
  ops.destroy = anycache::fuse_ops::Destroy;
  ops.getattr = anycache::fuse_ops::Getattr;
  ops.readdir = anycache::fuse_ops::Readdir;
  ops.statfs = anycache::fuse_ops::Statfs;
  ops.open = anycache::fuse_ops::Open;
  ops.create = anycache::fuse_ops::Create;
  ops.read = anycache::fuse_ops::Read;
  ops.write = anycache::fuse_ops::Write;
  ops.release = anycache::fuse_ops::Release;
  ops.truncate = anycache::fuse_ops::Truncate;
  ops.unlink = anycache::fuse_ops::Unlink;
  ops.rename = anycache::fuse_ops::Rename;
  ops.mkdir = anycache::fuse_ops::Mkdir;
  ops.rmdir = anycache::fuse_ops::Rmdir;
  ops.access = anycache::fuse_ops::Access;
  ops.chmod = anycache::fuse_ops::Chmod;
  ops.chown = anycache::fuse_ops::Chown;
  ops.utimens = anycache::fuse_ops::Utimens;

  LOG_INFO("Starting AnyCache FUSE at {}", cfg.fuse.mount_point);

  int fuse_argc = static_cast<int>(fuse_argv.size());
  int ret = fuse_main(fuse_argc, fuse_argv.data(), &ops, nullptr);
  return ret;
#else
  std::cerr << "AnyCache FUSE: libfuse3 not available at build time.\n"
            << "Please install libfuse3 and rebuild.\n";
  return 1;
#endif
}
