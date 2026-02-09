#include "common/config.h"
#include <gtest/gtest.h>

using namespace anycache;

TEST(ConfigTest, DefaultConfig) {
  Config cfg = Config::Default();
  EXPECT_EQ(cfg.master.port, 19999);
  EXPECT_EQ(cfg.worker.port, 29999);
  EXPECT_EQ(cfg.worker.page_size, kDefaultPageSize);
  EXPECT_FALSE(cfg.worker.tiers.empty());
  EXPECT_EQ(cfg.worker.tiers[0].type, TierType::kMemory);
}

TEST(ConfigTest, LoadFromYAML) {
  YAML::Node root;
  root["master"]["port"] = 20000;
  root["worker"]["port"] = 30000;
  root["worker"]["page_size"] = 2097152;

  Config cfg = Config::LoadFromYAML(root);
  EXPECT_EQ(cfg.master.port, 20000);
  EXPECT_EQ(cfg.worker.port, 30000);
  EXPECT_EQ(cfg.worker.page_size, 2097152u);
}

TEST(ConfigTest, LoadFromMissingFile) {
  // Should return defaults when file doesn't exist
  Config cfg = Config::LoadFromFile("/nonexistent/path/config.yaml");
  EXPECT_EQ(cfg.master.port, 19999);
}
