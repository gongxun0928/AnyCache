#include "common/config.h"
#include "common/logging.h"

#include <fstream>

namespace anycache {

Config Config::Default() {
  Config cfg;
  // Single memory tier by default
  TierConfig mem_tier;
  mem_tier.type = TierType::kMemory;
  mem_tier.path = "/tmp/anycache/mem";
  mem_tier.capacity_bytes = 1ULL * 1024 * 1024 * 1024; // 1 GB
  cfg.worker.tiers.push_back(mem_tier);
  return cfg;
}

Config Config::LoadFromFile(const std::string &path) {
  try {
    YAML::Node root = YAML::LoadFile(path);
    return LoadFromYAML(root);
  } catch (const std::exception &e) {
    LOG_WARN("Failed to load config from {}: {}, using defaults", path,
             e.what());
    return Default();
  }
}

Config Config::LoadFromYAML(const YAML::Node &root) {
  Config cfg = Default();

  if (auto master = root["master"]) {
    if (master["host"])
      cfg.master.host = master["host"].as<std::string>();
    if (master["port"])
      cfg.master.port = master["port"].as<int>();
    if (master["journal_dir"])
      cfg.master.journal_dir = master["journal_dir"].as<std::string>();
    if (master["heartbeat_timeout_ms"])
      cfg.master.worker_heartbeat_timeout_ms =
          master["heartbeat_timeout_ms"].as<int>();
    if (master["meta_db_dir"])
      cfg.master.meta_db_dir = master["meta_db_dir"].as<std::string>();
    if (master["metrics_port"])
      cfg.master.metrics_port = master["metrics_port"].as<int>();
  }

  if (auto worker = root["worker"]) {
    if (worker["host"])
      cfg.worker.host = worker["host"].as<std::string>();
    if (worker["port"])
      cfg.worker.port = worker["port"].as<int>();
    if (worker["master_address"])
      cfg.worker.master_address = worker["master_address"].as<std::string>();
    if (worker["page_size"])
      cfg.worker.page_size = worker["page_size"].as<size_t>();
    if (worker["block_size"])
      cfg.worker.block_size = worker["block_size"].as<size_t>();
    if (worker["metrics_port"])
      cfg.worker.metrics_port = worker["metrics_port"].as<int>();

    if (auto tiers = worker["tiers"]) {
      cfg.worker.tiers.clear();
      for (const auto &t : tiers) {
        TierConfig tc;
        std::string type_str = t["type"].as<std::string>("MEM");
        if (type_str == "MEM")
          tc.type = TierType::kMemory;
        else if (type_str == "SSD")
          tc.type = TierType::kSSD;
        else if (type_str == "HDD")
          tc.type = TierType::kHDD;
        tc.path = t["path"].as<std::string>("/tmp/anycache/data");
        tc.capacity_bytes = t["capacity_bytes"].as<size_t>(1073741824ULL);
        cfg.worker.tiers.push_back(tc);
      }
    }
  }

  if (auto fuse = root["fuse"]) {
    if (fuse["mount_point"])
      cfg.fuse.mount_point = fuse["mount_point"].as<std::string>();
    if (fuse["master_address"])
      cfg.fuse.master_address = fuse["master_address"].as<std::string>();
    if (fuse["direct_io"])
      cfg.fuse.direct_io = fuse["direct_io"].as<bool>();
  }

  if (auto rpc = root["rpc"]) {
    if (rpc["master_rpc_timeout_ms"])
      cfg.rpc.master_rpc_timeout_ms = rpc["master_rpc_timeout_ms"].as<int>();
    if (rpc["worker_rpc_timeout_ms"])
      cfg.rpc.worker_rpc_timeout_ms = rpc["worker_rpc_timeout_ms"].as<int>();
    if (rpc["internal_rpc_timeout_ms"])
      cfg.rpc.internal_rpc_timeout_ms =
          rpc["internal_rpc_timeout_ms"].as<int>();
  }

  if (auto s3 = root["s3"]) {
    if (s3["endpoint"])
      cfg.s3.endpoint = s3["endpoint"].as<std::string>();
    if (s3["bucket"])
      cfg.s3.bucket = s3["bucket"].as<std::string>();
    if (s3["region"])
      cfg.s3.region = s3["region"].as<std::string>();
    if (s3["access_key"])
      cfg.s3.access_key = s3["access_key"].as<std::string>();
    if (s3["secret_key"])
      cfg.s3.secret_key = s3["secret_key"].as<std::string>();
    if (s3["use_path_style"])
      cfg.s3.use_path_style = s3["use_path_style"].as<bool>();
  }

  return cfg;
}

} // namespace anycache
