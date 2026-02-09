#include "worker/block_store.h"
#include "worker/cache_manager.h"
#include "worker/page_store.h"
#include "worker/storage_tier.h"
#include <benchmark/benchmark.h>

#include <cstring>
#include <filesystem>
#include <random>

namespace fs = std::filesystem;

// ─── StorageTier benchmarks ──────────────────────────────────

static void BM_MemoryTierWrite(benchmark::State &state) {
  anycache::StorageTier tier(anycache::TierType::kMemory, "",
                             256 * 1024 * 1024);
  size_t block_size = state.range(0);

  anycache::BlockHandle handle;
  tier.AllocateBlock(1, block_size, &handle);

  std::vector<char> data(block_size, 'x');

  for (auto _ : state) {
    tier.WriteBlock(1, data.data(), block_size, 0);
  }
  state.SetBytesProcessed(state.iterations() * block_size);
  tier.RemoveBlock(1);
}
BENCHMARK(BM_MemoryTierWrite)->Arg(4096)->Arg(65536)->Arg(1048576);

static void BM_MemoryTierRead(benchmark::State &state) {
  anycache::StorageTier tier(anycache::TierType::kMemory, "",
                             256 * 1024 * 1024);
  size_t block_size = state.range(0);

  anycache::BlockHandle handle;
  tier.AllocateBlock(1, block_size, &handle);

  std::vector<char> data(block_size, 'x');
  tier.WriteBlock(1, data.data(), block_size, 0);

  std::vector<char> buf(block_size);
  for (auto _ : state) {
    tier.ReadBlock(1, buf.data(), block_size, 0);
  }
  state.SetBytesProcessed(state.iterations() * block_size);
  tier.RemoveBlock(1);
}
BENCHMARK(BM_MemoryTierRead)->Arg(4096)->Arg(65536)->Arg(1048576);

// ─── CacheManager LRU benchmarks ────────────────────────────

static void BM_LRU_Insert(benchmark::State &state) {
  anycache::CacheManager mgr(anycache::CacheManager::PolicyType::kLRU);
  uint64_t id = 1;
  for (auto _ : state) {
    mgr.OnBlockInsert(id++, 4096);
  }
}
BENCHMARK(BM_LRU_Insert);

static void BM_LRU_Access(benchmark::State &state) {
  anycache::CacheManager mgr(anycache::CacheManager::PolicyType::kLRU);
  int n = state.range(0);
  for (int i = 1; i <= n; ++i) {
    mgr.OnBlockInsert(i, 4096);
  }

  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(1, n);
  for (auto _ : state) {
    mgr.OnBlockAccess(dist(rng));
  }
}
BENCHMARK(BM_LRU_Access)->Arg(100)->Arg(10000)->Arg(100000);

static void BM_LRU_Evict(benchmark::State &state) {
  anycache::CacheManager mgr(anycache::CacheManager::PolicyType::kLRU);
  for (auto _ : state) {
    state.PauseTiming();
    for (int i = 1; i <= 1000; ++i) {
      mgr.OnBlockInsert(i, 4096);
    }
    state.ResumeTiming();
    mgr.GetEvictionCandidates(4096 * 500);
  }
}
BENCHMARK(BM_LRU_Evict);

// ─── PageStore benchmarks ────────────────────────────────────

static void BM_PageStoreWrite(benchmark::State &state) {
  anycache::PageStore store(4096, 10000);
  uint64_t page_idx = 0;
  char data[4096];
  std::memset(data, 'a', sizeof(data));

  for (auto _ : state) {
    store.WritePage(1, page_idx++, data, sizeof(data));
  }
  state.SetBytesProcessed(state.iterations() * 4096);
}
BENCHMARK(BM_PageStoreWrite);

static void BM_PageStoreRead(benchmark::State &state) {
  anycache::PageStore store(4096, 10000);
  char data[4096];
  std::memset(data, 'a', sizeof(data));

  // Pre-populate
  int n = 1000;
  for (int i = 0; i < n; ++i) {
    store.WritePage(1, i, data, sizeof(data));
  }

  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, n - 1);
  char buf[4096];
  size_t bytes_read;
  for (auto _ : state) {
    store.ReadPage(1, dist(rng), buf, &bytes_read);
  }
  state.SetBytesProcessed(state.iterations() * 4096);
}
BENCHMARK(BM_PageStoreRead);

// ─── BlockStore benchmarks ───────────────────────────────────

static void BM_BlockStoreCreateWrite(benchmark::State &state) {
  fs::path test_dir = fs::temp_directory_path() / "anycache_bench";
  fs::remove_all(test_dir);
  fs::create_directories(test_dir);

  anycache::BlockStore::Options opts;
  anycache::TierConfig tc;
  tc.type = anycache::TierType::kMemory;
  tc.path = "";
  tc.capacity_bytes = 512 * 1024 * 1024;
  opts.tiers.push_back(tc);
  opts.meta_db_path = (test_dir / "meta").string();

  anycache::BlockStore store(opts);

  size_t block_size = state.range(0);
  std::vector<char> data(block_size, 'x');

  uint32_t block_idx = 0;
  for (auto _ : state) {
    anycache::BlockId id = anycache::MakeBlockId(1, block_idx++);
    store.CreateBlock(id, block_size);
    store.WriteBlock(id, data.data(), block_size, 0);
  }
  state.SetBytesProcessed(state.iterations() * block_size);

  fs::remove_all(test_dir);
}
BENCHMARK(BM_BlockStoreCreateWrite)->Arg(4096)->Arg(65536);

BENCHMARK_MAIN();
