#pragma once

#include "common/status.h"
#include "master/journal/journal_writer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace anycache {

// Simplified Raft state for Master HA.
// In production, integrate braft or a full Raft library.
class RaftJournal {
public:
  enum class Role { kFollower, kCandidate, kLeader };

  struct Config {
    uint64_t node_id = 1;
    std::vector<std::string> peers; // "host:port" of other masters
    std::string journal_dir = "/tmp/anycache/raft";
    int election_timeout_ms = 3000;
    int heartbeat_interval_ms = 1000;
  };

  explicit RaftJournal(const Config &config);
  ~RaftJournal();

  Status Start();
  void Stop();

  // Append an entry (only succeeds on leader)
  Status Propose(const JournalEntryData &entry);

  // Check if this node is the leader
  bool IsLeader() const { return role_ == Role::kLeader; }
  Role GetRole() const { return role_; }
  uint64_t GetTerm() const { return current_term_; }

  // Set callback for when entries are committed
  void SetCommitCallback(std::function<void(const JournalEntryData &)> cb) {
    commit_cb_ = std::move(cb);
  }

private:
  void ElectionLoop();
  void HeartbeatLoop();

  Config config_;
  std::unique_ptr<JournalWriter> journal_writer_;

  std::atomic<Role> role_{Role::kFollower};
  std::atomic<uint64_t> current_term_{0};
  std::atomic<uint64_t> voted_for_{0};
  std::atomic<uint64_t> commit_index_{0};
  std::atomic<uint64_t> last_applied_{0};

  std::function<void(const JournalEntryData &)> commit_cb_;

  std::atomic<bool> running_{false};
  std::thread election_thread_;
  std::thread heartbeat_thread_;
  mutable std::mutex mu_;
};

} // namespace anycache
