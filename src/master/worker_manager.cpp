#include "master/worker_manager.h"
#include "common/logging.h"

namespace anycache {

WorkerManager::WorkerManager(int heartbeat_timeout_ms)
    : heartbeat_timeout_ms_(heartbeat_timeout_ms) {}

int64_t WorkerManager::NowMs() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Status WorkerManager::RegisterWorker(const std::string &address,
                                     uint64_t capacity, uint64_t used,
                                     WorkerId *out_id) {
  std::lock_guard<std::mutex> lock(mu_);

  // Check for re-registration
  for (auto &[id, w] : workers_) {
    if (w.address == address) {
      w.capacity_bytes = capacity;
      w.used_bytes = used;
      w.last_heartbeat_ms = NowMs();
      w.alive = true;
      *out_id = id;
      LOG_INFO("Worker re-registered: id={}, address={}", id, address);
      return Status::OK();
    }
  }

  WorkerId id = next_id_.fetch_add(1);
  WorkerState ws;
  ws.id = id;
  ws.address = address;
  ws.capacity_bytes = capacity;
  ws.used_bytes = used;
  ws.last_heartbeat_ms = NowMs();
  ws.alive = true;

  workers_[id] = ws;
  *out_id = id;
  LOG_INFO("Worker registered: id={}, address={}, capacity={}MB", id, address,
           capacity / (1024 * 1024));
  return Status::OK();
}

Status WorkerManager::Heartbeat(WorkerId id, uint64_t capacity, uint64_t used) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = workers_.find(id);
  if (it == workers_.end())
    return Status::NotFound("worker not registered");

  it->second.capacity_bytes = capacity;
  it->second.used_bytes = used;
  it->second.last_heartbeat_ms = NowMs();
  it->second.alive = true;
  return Status::OK();
}

Status WorkerManager::GetWorker(WorkerId id, WorkerState *out) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = workers_.find(id);
  if (it == workers_.end())
    return Status::NotFound("worker not found");
  *out = it->second;
  return Status::OK();
}

std::vector<WorkerState> WorkerManager::GetLiveWorkers() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<WorkerState> result;
  for (auto &[_, w] : workers_) {
    if (w.alive)
      result.push_back(w);
  }
  return result;
}

Status WorkerManager::SelectWorkerForWrite(WorkerId *out_id) const {
  std::lock_guard<std::mutex> lock(mu_);

  WorkerId best_id = kInvalidWorkerId;
  uint64_t best_avail = 0;

  for (auto &[id, w] : workers_) {
    if (!w.alive)
      continue;
    uint64_t avail = w.capacity_bytes - w.used_bytes;
    if (avail > best_avail) {
      best_avail = avail;
      best_id = id;
    }
  }

  if (best_id == kInvalidWorkerId) {
    return Status::Unavailable("no workers available");
  }
  *out_id = best_id;
  return Status::OK();
}

std::vector<WorkerId> WorkerManager::CheckHeartbeats() {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t now = NowMs();
  std::vector<WorkerId> dead;

  for (auto &[id, w] : workers_) {
    if (w.alive && (now - w.last_heartbeat_ms) > heartbeat_timeout_ms_) {
      w.alive = false;
      dead.push_back(id);
      LOG_WARN("Worker {} ({}): heartbeat timeout", id, w.address);
    }
  }
  return dead;
}

size_t WorkerManager::GetWorkerCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  size_t count = 0;
  for (auto &[_, w] : workers_) {
    if (w.alive)
      count++;
  }
  return count;
}

} // namespace anycache
