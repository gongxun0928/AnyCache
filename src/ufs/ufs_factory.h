#pragma once

#include "common/config.h"
#include "ufs/ufs.h"

#include <memory>
#include <string>

namespace anycache {

class UfsFactory {
public:
  // Create a UFS based on URI scheme
  // e.g., "file:///path" -> LocalUnderFileSystem
  //       "s3://bucket/prefix" -> S3UnderFileSystem
  static std::unique_ptr<UnderFileSystem>
  Create(const std::string &uri, const Config &config = Config::Default());

  // Create based on explicit scheme
  static std::unique_ptr<UnderFileSystem>
  CreateByScheme(const std::string &scheme, const std::string &path,
                 const Config &config = Config::Default());

  // Parse URI into scheme + path
  static void ParseUri(const std::string &uri, std::string *scheme,
                       std::string *path);
};

} // namespace anycache
