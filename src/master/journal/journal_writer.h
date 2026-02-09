#pragma once

#include "common/status.h"

#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace anycache {

// Serialized journal entry (opaque bytes for the writer)
struct JournalEntryData {
  uint64_t sequence_number;
  int64_t timestamp_ms;
  uint32_t type;
  std::string payload;

  std::string Serialize() const;
  static JournalEntryData Deserialize(const std::string &data);
};

// JournalWriter writes operation logs to disk for Master crash recovery.
// Each entry is appended to a write-ahead log file.
class JournalWriter {
public:
  explicit JournalWriter(const std::string &journal_dir);
  ~JournalWriter();

  Status Open();
  Status Close();

  // Append an entry to the journal
  Status Append(const JournalEntryData &entry);

  // Flush all buffered entries to disk
  Status Flush();

  // Replay the journal (for recovery), calling `callback` for each entry
  Status Replay(std::function<void(const JournalEntryData &)> callback);

  // Truncate the journal after a snapshot
  Status Truncate(uint64_t up_to_sequence);

  uint64_t GetNextSequenceNumber() const { return next_seq_; }

private:
  std::string journal_dir_;
  std::string journal_file_path_;

  std::mutex mu_;
  std::ofstream writer_;
  std::atomic<uint64_t> next_seq_{1};
};

} // namespace anycache
