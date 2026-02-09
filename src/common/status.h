#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace anycache {

enum class StatusCode : uint8_t {
  kOk = 0,
  kNotFound = 1,
  kAlreadyExists = 2,
  kInvalidArgument = 3,
  kIOError = 4,
  kPermissionDenied = 5,
  kNotImplemented = 6,
  kResourceExhausted = 7,
  kUnavailable = 8,
  kInternal = 9,
  kCancelled = 10,
  kDeadlineExceeded = 11,
};

class Status {
public:
  Status() : code_(StatusCode::kOk) {}
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  // Factory methods
  static Status OK() { return Status(); }
  static Status NotFound(std::string_view msg) {
    return Status(StatusCode::kNotFound, std::string(msg));
  }
  static Status AlreadyExists(std::string_view msg) {
    return Status(StatusCode::kAlreadyExists, std::string(msg));
  }
  static Status InvalidArgument(std::string_view msg) {
    return Status(StatusCode::kInvalidArgument, std::string(msg));
  }
  static Status IOError(std::string_view msg) {
    return Status(StatusCode::kIOError, std::string(msg));
  }
  static Status PermissionDenied(std::string_view msg) {
    return Status(StatusCode::kPermissionDenied, std::string(msg));
  }
  static Status NotImplemented(std::string_view msg) {
    return Status(StatusCode::kNotImplemented, std::string(msg));
  }
  static Status ResourceExhausted(std::string_view msg) {
    return Status(StatusCode::kResourceExhausted, std::string(msg));
  }
  static Status Unavailable(std::string_view msg) {
    return Status(StatusCode::kUnavailable, std::string(msg));
  }
  static Status Internal(std::string_view msg) {
    return Status(StatusCode::kInternal, std::string(msg));
  }

  bool ok() const { return code_ == StatusCode::kOk; }
  bool IsNotFound() const { return code_ == StatusCode::kNotFound; }
  bool IsAlreadyExists() const { return code_ == StatusCode::kAlreadyExists; }
  bool IsIOError() const { return code_ == StatusCode::kIOError; }

  StatusCode code() const { return code_; }
  const std::string &message() const { return message_; }

  std::string ToString() const;

private:
  StatusCode code_;
  std::string message_;
};

// Convenience macro
#define RETURN_IF_ERROR(expr)                                                  \
  do {                                                                         \
    auto _s = (expr);                                                          \
    if (!_s.ok())                                                              \
      return _s;                                                               \
  } while (0)

} // namespace anycache
