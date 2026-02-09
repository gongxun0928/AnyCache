#include "ufs/ufs_factory.h"
#include "common/logging.h"
#include "ufs/local_ufs.h"

#ifdef ANYCACHE_HAS_S3
#include "ufs/s3_ufs.h"
#endif

namespace anycache {

void UfsFactory::ParseUri(const std::string &uri, std::string *scheme,
                          std::string *path) {
  auto pos = uri.find("://");
  if (pos == std::string::npos) {
    // No scheme, treat as local file path
    *scheme = "file";
    *path = uri;
  } else {
    *scheme = uri.substr(0, pos);
    *path = uri.substr(pos + 3);
  }
}

std::unique_ptr<UnderFileSystem> UfsFactory::Create(const std::string &uri,
                                                    const Config &config) {
  std::string scheme, path;
  ParseUri(uri, &scheme, &path);
  return CreateByScheme(scheme, path, config);
}

std::unique_ptr<UnderFileSystem>
UfsFactory::CreateByScheme(const std::string &scheme, const std::string &path,
                           const Config &config) {
  if (scheme == "file" || scheme == "local") {
    return std::make_unique<LocalUnderFileSystem>(path);
  }
#ifdef ANYCACHE_HAS_S3
  if (scheme == "s3") {
    S3Config s3_cfg = config.s3;
    std::string prefix;
    if (!path.empty()) {
      auto slash = path.find('/');
      if (slash != std::string::npos) {
        s3_cfg.bucket = path.substr(0, slash);
        prefix = path.substr(slash + 1);
      } else {
        s3_cfg.bucket = path;
      }
    }
    return std::make_unique<S3UnderFileSystem>(s3_cfg, prefix);
  }
#endif
  LOG_ERROR("Unknown UFS scheme: {}", scheme);
  return nullptr;
}

} // namespace anycache
