# miniocpp-patch.cmake
# Patch minio-cpp's CMakeLists.txt for FetchContent sub-project usage:
#   1. Replace find_package() calls with target existence checks
#      (the targets are already created by our FetchMiniocpp.cmake)
#   2. Wrap install() calls so they only run for top-level project
# SRC_DIR is passed via -DSRC_DIR=<SOURCE_DIR>

file(READ "${SRC_DIR}/CMakeLists.txt" content)

# ── 1. Replace find_package calls for dependencies we provide via FetchContent ──

# Replace: find_package(unofficial-curlpp CONFIG REQUIRED)
string(REPLACE
    "find_package(unofficial-curlpp CONFIG REQUIRED)"
    "# [patched] unofficial-curlpp provided by parent project"
    content "${content}")

# Replace: find_package(unofficial-inih CONFIG REQUIRED)
string(REPLACE
    "find_package(unofficial-inih CONFIG REQUIRED)"
    "# [patched] unofficial-inih provided by parent project"
    content "${content}")

# Replace: find_package(nlohmann_json CONFIG REQUIRED)
string(REPLACE
    "find_package(nlohmann_json CONFIG REQUIRED)"
    "# [patched] nlohmann_json provided by parent project"
    content "${content}")

# Replace: find_package(pugixml CONFIG REQUIRED)
string(REPLACE
    "find_package(pugixml CONFIG REQUIRED)"
    "# [patched] pugixml provided by parent project"
    content "${content}")

# OpenSSL and ZLIB are found by the parent project already,
# but we keep their find_package (they use system packages with proper config).

# ── 2. Wrap install() section with top-level guard ──

string(REPLACE
    "configure_package_config_file("
    "if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)\nconfigure_package_config_file("
    content "${content}")

string(REPLACE
    "install(FILES \${CMAKE_CURRENT_BINARY_DIR}/miniocpp.pc DESTINATION \${CMAKE_INSTALL_LIBDIR}/pkgconfig)"
    "install(FILES \${CMAKE_CURRENT_BINARY_DIR}/miniocpp.pc DESTINATION \${CMAKE_INSTALL_LIBDIR}/pkgconfig)\nendif()"
    content "${content}")

file(WRITE "${SRC_DIR}/CMakeLists.txt" "${content}")
message(STATUS "[miniocpp-patch] Patched CMakeLists.txt for FetchContent usage")
