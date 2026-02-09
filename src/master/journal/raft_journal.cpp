#include "master/journal/raft_journal.h"
#include "common/logging.h"

#include <chrono>
#include <random>

namespace anycache {

RaftJournal::RaftJournal(const Config &config) : config_(config) {
  journal_writer_ = std::make_unique<JournalWriter>(config_.journal_dir);
}

RaftJournal::~RaftJournal() { Stop(); }

Status RaftJournal::Start() {
  RETURN_IF_ERROR(journal_writer_->Open());

  running_ = true;

  if (config_.peers.empty()) {
    // Single-node mode: immediately become leader
    role_ = Role::kLeader;
    current_term_ = 1;
    LOG_INFO("RaftJournal: single-node mode, assuming leadership (term=1)");
  } else {
    role_ = Role::kFollower;
    election_thread_ = std::thread(&RaftJournal::ElectionLoop, this);
    heartbeat_thread_ = std::thread(&RaftJournal::HeartbeatLoop, this);
    LOG_INFO("RaftJournal: multi-node mode, starting as follower "
             "(node_id={}, peers={})",
             config_.node_id, config_.peers.size());
  }

  return Status::OK();
}

void RaftJournal::Stop() {
  running_ = false;
  if (election_thread_.joinable())
    election_thread_.join();
  if (heartbeat_thread_.joinable())
    heartbeat_thread_.join();
  journal_writer_->Close();
}

Status RaftJournal::Propose(const JournalEntryData &entry) {
  if (role_ != Role::kLeader) {
    return Status::Unavailable("not the leader");
  }

  // Assign sequence number
  JournalEntryData e = entry;
  e.sequence_number = journal_writer_->GetNextSequenceNumber();
  e.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

  // Write to local journal
  RETURN_IF_ERROR(journal_writer_->Append(e));
  RETURN_IF_ERROR(journal_writer_->Flush());

  // In full Raft: replicate to followers, wait for majority
  // For now: single-node commit
  commit_index_ = e.sequence_number;
  last_applied_ = e.sequence_number;

  if (commit_cb_) {
    commit_cb_(e);
  }

  return Status::OK();
}

void RaftJournal::ElectionLoop() {
  std::random_device rd;
  std::mt19937 gen(rd());

  while (running_) {
    // Randomize election timeout to avoid split-brain
    int timeout = config_.election_timeout_ms +
                  std::uniform_int_distribution<int>(0, 1000)(gen);
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout));

    if (!running_)
      break;

    if (role_ == Role::kFollower) {
      // In full Raft: check if we received a heartbeat recently.
      // If not, start an election.
      // Simplified: just log.
      LOG_DEBUG("RaftJournal: election timeout (would start election)");

      // For now, if single-node, become leader
      if (config_.peers.empty()) {
        role_ = Role::kLeader;
        current_term_++;
      }
    }
  }
}

void RaftJournal::HeartbeatLoop() {
  while (running_) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.heartbeat_interval_ms));

    if (!running_)
      break;

    if (role_ == Role::kLeader) {
      // In full Raft: send AppendEntries RPCs to all peers
      LOG_TRACE("RaftJournal: heartbeat (term={})", current_term_.load());
    }
  }
}

} // namespace anycache
