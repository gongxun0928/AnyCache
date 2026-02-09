#pragma once

#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "common/types.h"

namespace anycache {

struct TierConfig {
  TierType type;
  std::string path;
  size_t capacity_bytes;
};

struct WorkerConfig {
  std::string host = "0.0.0.0";
  int port = 29999;
  std::string master_address = "localhost:19999";
  std::vector<TierConfig> tiers;
  size_t page_size = kDefaultPageSize;
  size_t block_size = kDefaultBlockSize;
  int metrics_port = 9202; // Prometheus /metrics HTTP port; 0 = disabled
};

struct MasterConfig {
  std::string host = "0.0.0.0";
  int port = 19999;
  std::string journal_dir = "/tmp/anycache/journal";
  int worker_heartbeat_timeout_ms = 30000;
  std::string meta_db_dir = "/tmp/anycache/master/meta";
  int metrics_port = 9201; // Prometheus /metrics HTTP port; 0 = disabled
};

struct FuseConfig {
  std::string mount_point = "/mnt/anycache";
  std::string master_address = "localhost:19999";
  bool direct_io = false;
  size_t max_read = 131072;
};

struct S3Config {
  std::string endpoint;
  std::string bucket;
  std::string region = "us-east-1";
  std::string access_key;
  std::string secret_key;
  bool use_path_style = false;
};

// RPC timeout settings (milliseconds).
// Different communication paths may have different latency profiles:
//   - master_rpc_timeout_ms:   Client → Master  (metadata ops, lightweight)
//   - worker_rpc_timeout_ms:   Client → Worker  (block read/write, heavier)
//   - internal_rpc_timeout_ms: Worker → Master  (register, heartbeat, report)
// Set to 0 to disable deadline (not recommended in production).
struct RpcConfig {
  int master_rpc_timeout_ms = 10000;   // 10 s
  int worker_rpc_timeout_ms = 30000;   // 30 s
  int internal_rpc_timeout_ms = 10000; // 10 s
};

struct Config {
  MasterConfig master;
  WorkerConfig worker;
  FuseConfig fuse;
  S3Config s3;
  RpcConfig rpc;

  static Config LoadFromFile(const std::string &path);
  static Config LoadFromYAML(const YAML::Node &node);
  static Config Default();
};

} // namespace anycache
