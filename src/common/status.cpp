#include "common/status.h"

#include <sstream>

namespace anycache {

std::string Status::ToString() const {
  if (ok())
    return "OK";

  std::ostringstream oss;
  switch (code_) {
  case StatusCode::kOk:
    oss << "OK";
    break;
  case StatusCode::kNotFound:
    oss << "NotFound";
    break;
  case StatusCode::kAlreadyExists:
    oss << "AlreadyExists";
    break;
  case StatusCode::kInvalidArgument:
    oss << "InvalidArgument";
    break;
  case StatusCode::kIOError:
    oss << "IOError";
    break;
  case StatusCode::kPermissionDenied:
    oss << "PermissionDenied";
    break;
  case StatusCode::kNotImplemented:
    oss << "NotImplemented";
    break;
  case StatusCode::kResourceExhausted:
    oss << "ResourceExhausted";
    break;
  case StatusCode::kUnavailable:
    oss << "Unavailable";
    break;
  case StatusCode::kInternal:
    oss << "Internal";
    break;
  case StatusCode::kCancelled:
    oss << "Cancelled";
    break;
  case StatusCode::kDeadlineExceeded:
    oss << "DeadlineExceeded";
    break;
  }
  if (!message_.empty()) {
    oss << ": " << message_;
  }
  return oss.str();
}

} // namespace anycache
