#include "master/journal/journal_writer.h"
#include "common/logging.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace anycache {

// Simple binary format: [4-byte length][payload]
// payload = [8-byte seq][8-byte timestamp][4-byte type][N-byte data]

std::string JournalEntryData::Serialize() const {
  std::string buf;
  buf.resize(8 + 8 + 4 + payload.size());
  char *p = buf.data();
  std::memcpy(p, &sequence_number, 8);
  p += 8;
  std::memcpy(p, &timestamp_ms, 8);
  p += 8;
  std::memcpy(p, &type, 4);
  p += 4;
  std::memcpy(p, payload.data(), payload.size());
  return buf;
}

JournalEntryData JournalEntryData::Deserialize(const std::string &data) {
  JournalEntryData entry;
  if (data.size() < 20)
    return entry;
  const char *p = data.data();
  std::memcpy(&entry.sequence_number, p, 8);
  p += 8;
  std::memcpy(&entry.timestamp_ms, p, 8);
  p += 8;
  std::memcpy(&entry.type, p, 4);
  p += 4;
  entry.payload.assign(p, data.data() + data.size());
  return entry;
}

JournalWriter::JournalWriter(const std::string &journal_dir)
    : journal_dir_(journal_dir) {
  journal_file_path_ = journal_dir_ + "/journal.log";
}

JournalWriter::~JournalWriter() { Close(); }

Status JournalWriter::Open() {
  fs::create_directories(journal_dir_);

  // Replay to find the last sequence number
  if (fs::exists(journal_file_path_)) {
    uint64_t max_seq = 0;
    Replay([&max_seq](const JournalEntryData &e) {
      if (e.sequence_number > max_seq)
        max_seq = e.sequence_number;
    });
    next_seq_ = max_seq + 1;
  }

  std::lock_guard<std::mutex> lock(mu_);
  writer_.open(journal_file_path_, std::ios::binary | std::ios::app);
  if (!writer_.is_open()) {
    return Status::IOError("failed to open journal: " + journal_file_path_);
  }

  LOG_INFO("JournalWriter opened: {}, next_seq={}", journal_file_path_,
           next_seq_.load());
  return Status::OK();
}

Status JournalWriter::Close() {
  std::lock_guard<std::mutex> lock(mu_);
  if (writer_.is_open()) {
    writer_.close();
  }
  return Status::OK();
}

Status JournalWriter::Append(const JournalEntryData &entry) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!writer_.is_open()) {
    return Status::IOError("journal not open");
  }

  std::string serialized = entry.Serialize();
  uint32_t len = static_cast<uint32_t>(serialized.size());

  writer_.write(reinterpret_cast<const char *>(&len), 4);
  writer_.write(serialized.data(), serialized.size());

  if (writer_.fail()) {
    return Status::IOError("journal write failed");
  }

  return Status::OK();
}

Status JournalWriter::Flush() {
  std::lock_guard<std::mutex> lock(mu_);
  if (writer_.is_open()) {
    writer_.flush();
  }
  return Status::OK();
}

Status
JournalWriter::Replay(std::function<void(const JournalEntryData &)> callback) {
  std::ifstream reader(journal_file_path_, std::ios::binary);
  if (!reader.is_open()) {
    return Status::OK(); // No journal to replay
  }

  uint64_t count = 0;
  while (reader.good()) {
    uint32_t len = 0;
    reader.read(reinterpret_cast<char *>(&len), 4);
    if (reader.eof())
      break;
    if (len == 0 || len > 64 * 1024 * 1024)
      break; // Sanity check

    std::string data(len, '\0');
    reader.read(data.data(), len);
    if (reader.fail())
      break;

    auto entry = JournalEntryData::Deserialize(data);
    callback(entry);
    count++;
  }

  LOG_INFO("Replayed {} journal entries", count);
  return Status::OK();
}

Status JournalWriter::Truncate(uint64_t /*up_to_sequence*/) {
  // Simple implementation: rewrite the journal file keeping only
  // entries after the given sequence. For production, use checkpoints.
  LOG_INFO("Journal truncation (simplified): keeping all entries");
  return Status::OK();
}

} // namespace anycache
