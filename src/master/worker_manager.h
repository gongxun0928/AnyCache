#pragma once

#include "common/status.h"
#include "common/types.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace anycache {

struct WorkerState {
  WorkerId id = kInvalidWorkerId;
  std::string address;
  uint64_t capacity_bytes = 0;
  uint64_t used_bytes = 0;
  int64_t last_heartbeat_ms = 0;
  bool alive = true;
};

// WorkerManager tracks registered workers and their health.
class WorkerManager {
public:
  explicit WorkerManager(int heartbeat_timeout_ms = 30000);

  // Register a new worker, returns assigned worker id
  Status RegisterWorker(const std::string &address, uint64_t capacity,
                        uint64_t used, WorkerId *out_id);

  // Process a heartbeat from a worker
  Status Heartbeat(WorkerId id, uint64_t capacity, uint64_t used);

  // Get a worker by ID
  Status GetWorker(WorkerId id, WorkerState *out) const;

  // Get all live workers
  std::vector<WorkerState> GetLiveWorkers() const;

  // Select a worker for writing (simple round-robin or least-used)
  Status SelectWorkerForWrite(WorkerId *out_id) const;

  // Check for timed-out workers, returns IDs of dead workers
  std::vector<WorkerId> CheckHeartbeats();

  size_t GetWorkerCount() const;

private:
  int64_t NowMs() const;

  mutable std::mutex mu_;
  std::unordered_map<WorkerId, WorkerState> workers_;
  std::atomic<WorkerId> next_id_{1};
  int heartbeat_timeout_ms_;
};

} // namespace anycache
