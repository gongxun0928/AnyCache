cmake_minimum_required(VERSION 3.20)
project(anycache VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(ANYCACHE_BUILD_TESTS "Build tests" ON)
option(ANYCACHE_BUILD_BENCHMARKS "Build benchmarks" ON)
option(ANYCACHE_ENABLE_FUSE "Enable FUSE support" ON)
option(ANYCACHE_ENABLE_S3 "Enable S3 UFS support" ON)
option(ANYCACHE_ENABLE_RAFT "Enable Raft HA support" ON)

# ─── Dependencies ─────────────────────────────────────────────
find_package(Threads REQUIRED)

# spdlog
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    include(FetchContent)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.13.0)
    FetchContent_MakeAvailable(spdlog)
endif()

# yaml-cpp
find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
    include(FetchContent)
    FetchContent_Declare(yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG yaml-cpp-0.9.0
        )
    FetchContent_MakeAvailable(yaml-cpp)
endif()

# RocksDB (for block metadata persistence)
find_package(RocksDB QUIET)
if(NOT RocksDB_FOUND)
    include(FetchContent)
    FetchContent_Declare(rocksdb
        GIT_REPOSITORY https://github.com/facebook/rocksdb.git
        GIT_TAG v9.8.4
    )
    set(WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
    set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(rocksdb)
    set(ANYCACHE_HAS_ROCKSDB ON)
else()
    set(ANYCACHE_HAS_ROCKSDB ON)
endif()

# gRPC + Protobuf (required)
# Install via: brew install grpc protobuf  (macOS)
#              apt install libgrpc++-dev protobuf-compiler-grpc  (Ubuntu)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# FUSE
if(ANYCACHE_ENABLE_FUSE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FUSE3 fuse3)
    endif()
    if(FUSE3_FOUND)
        set(ANYCACHE_HAS_FUSE ON)
    else()
        message(STATUS "libfuse3 not found; FUSE module will not be built")
        set(ANYCACHE_HAS_FUSE OFF)
    endif()
endif()

# S3 support via minio-cpp
if(ANYCACHE_ENABLE_S3)
    find_package(miniocpp QUIET)
    if(NOT miniocpp_FOUND)
        message(STATUS "minio-cpp not found via find_package; using FetchContent")
        include(cmake/FetchMiniocpp.cmake)
    endif()
    set(ANYCACHE_HAS_S3 ON)
endif()

# ─── Proto generation ──────────────────────────────────────────
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")

set(PROTO_SRCS "")
set(PROTO_HDRS "")
set(GRPC_SRCS "")
set(GRPC_HDRS "")

foreach(proto ${PROTO_FILES})
    get_filename_component(proto_name ${proto} NAME_WE)
    list(APPEND PROTO_SRCS "${PROTO_GEN_DIR}/${proto_name}.pb.cc")
    list(APPEND PROTO_HDRS "${PROTO_GEN_DIR}/${proto_name}.pb.h")
    list(APPEND GRPC_SRCS "${PROTO_GEN_DIR}/${proto_name}.grpc.pb.cc")
    list(APPEND GRPC_HDRS "${PROTO_GEN_DIR}/${proto_name}.grpc.pb.h")
endforeach()

find_program(PROTOC protoc)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${PROTOC}
        --proto_path=${PROTO_DIR}
        --cpp_out=${PROTO_GEN_DIR}
        --grpc_out=${PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        ${PROTO_FILES}
    DEPENDS ${PROTO_FILES}
    COMMENT "Generating protobuf/gRPC sources"
)

add_library(anycache_proto ${PROTO_SRCS} ${GRPC_SRCS})
target_include_directories(anycache_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(anycache_proto PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

# ─── Common library ───────────────────────────────────────────
add_library(anycache_common
    src/common/config.cpp
    src/common/status.cpp
    src/common/metrics_server.cpp
)
target_include_directories(anycache_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(anycache_common PUBLIC
    spdlog::spdlog
    yaml-cpp::yaml-cpp
    Threads::Threads
)

# ─── UFS library ──────────────────────────────────────────────
set(UFS_SOURCES
    src/ufs/ufs_factory.cpp
    src/ufs/local_ufs.cpp
    src/ufs/s3_ufs.cpp
)

add_library(anycache_ufs ${UFS_SOURCES})
target_link_libraries(anycache_ufs PUBLIC anycache_common)
if(ANYCACHE_HAS_S3)
    target_link_libraries(anycache_ufs PUBLIC miniocpp::miniocpp)
    target_compile_definitions(anycache_ufs PUBLIC ANYCACHE_HAS_S3=1)
endif()

# ─── Worker / Cache engine library ───────────────────────────
add_library(anycache_worker
    src/worker/storage_tier.cpp
    src/worker/block_store.cpp
    src/worker/page_store.cpp
    src/worker/cache_manager.cpp
    src/worker/meta_store.cpp
    src/worker/data_mover.cpp
    src/worker/worker_server.cpp
    src/worker/worker_service_impl.cpp
    src/worker/master_client.cpp
)
target_link_libraries(anycache_worker PUBLIC anycache_common anycache_ufs anycache_proto)
if(ANYCACHE_HAS_ROCKSDB)
    if(RocksDB_FOUND)
        target_link_libraries(anycache_worker PUBLIC RocksDB::rocksdb)
    else()
        target_link_libraries(anycache_worker PUBLIC rocksdb)
        target_include_directories(anycache_worker PUBLIC ${rocksdb_SOURCE_DIR}/include)
    endif()
    target_compile_definitions(anycache_worker PUBLIC ANYCACHE_HAS_ROCKSDB=1)
endif()

# ─── Master library ──────────────────────────────────────────
add_library(anycache_master
    src/master/inode_tree.cpp
    src/master/inode_store.cpp
    src/master/block_master.cpp
    src/master/file_system_master.cpp
    src/master/worker_manager.cpp
    src/master/mount_table.cpp
    src/master/master_server.cpp
    src/master/master_service_impl.cpp
    src/master/journal/journal_writer.cpp
    src/master/journal/raft_journal.cpp
)
target_link_libraries(anycache_master PUBLIC anycache_common anycache_ufs anycache_proto)
if(ANYCACHE_HAS_ROCKSDB)
    if(RocksDB_FOUND)
        target_link_libraries(anycache_master PUBLIC RocksDB::rocksdb)
    else()
        target_link_libraries(anycache_master PUBLIC rocksdb)
        target_include_directories(anycache_master PUBLIC ${rocksdb_SOURCE_DIR}/include)
    endif()
endif()

# ─── Client library (standalone: no master/worker impl) ─────────
add_library(anycache_client
    src/client/file_system_client.cpp
    src/client/block_client.cpp
    src/client/client_config.cpp
)
target_include_directories(anycache_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(anycache_client PUBLIC anycache_common anycache_proto yaml-cpp::yaml-cpp)

# ─── Client cache (optional: local page cache, depends on worker) ─
add_library(anycache_client_cache
    src/client/cache/cache_client.cpp
)
target_include_directories(anycache_client_cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(anycache_client_cache PUBLIC anycache_client anycache_worker)

# ─── Executables ──────────────────────────────────────────────
add_executable(anycache-master tools/anycache_master_main.cpp)
target_link_libraries(anycache-master PRIVATE anycache_master anycache_worker)

add_executable(anycache-worker tools/anycache_worker_main.cpp)
target_link_libraries(anycache-worker PRIVATE anycache_worker)

add_executable(anycache-cli tools/anycache_cli_main.cpp)
target_link_libraries(anycache-cli PRIVATE anycache_client anycache_common)

# FUSE
if(ANYCACHE_HAS_FUSE)
    add_executable(anycache-fuse
        src/fuse/fuse_main.cpp
        src/fuse/fuse_operations.cpp
    )
    target_link_libraries(anycache-fuse PRIVATE
        anycache_client anycache_common
        ${FUSE3_LIBRARIES}
    )
    target_include_directories(anycache-fuse PRIVATE ${FUSE3_INCLUDE_DIRS})
    target_compile_definitions(anycache-fuse PRIVATE ANYCACHE_HAS_FUSE=1)
endif()

# ─── Tests ────────────────────────────────────────────────────
if(ANYCACHE_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0)
        FetchContent_MakeAvailable(googletest)
    endif()
    enable_testing()

    # Common tests
    add_executable(common_test tests/common/status_test.cpp tests/common/config_test.cpp)
    target_link_libraries(common_test PRIVATE anycache_common GTest::gtest_main)
    add_test(NAME common_test COMMAND common_test)

    # UFS tests
    add_executable(ufs_test tests/ufs/local_ufs_test.cpp)
    target_link_libraries(ufs_test PRIVATE anycache_ufs GTest::gtest_main)
    add_test(NAME ufs_test COMMAND ufs_test)

    # Worker / cache tests
    add_executable(worker_test
        tests/worker/storage_tier_test.cpp
        tests/worker/block_store_test.cpp
        tests/worker/page_store_test.cpp
        tests/worker/cache_manager_test.cpp
        tests/worker/data_mover_test.cpp
        tests/worker/worker_service_impl_test.cpp
    )
    target_link_libraries(worker_test PRIVATE anycache_worker GTest::gtest_main)
    add_test(NAME worker_test COMMAND worker_test)

    # Client cache tests
    add_executable(client_cache_test
        tests/client/cache_client_test.cpp
    )
    target_link_libraries(client_cache_test PRIVATE anycache_client_cache GTest::gtest_main)
    add_test(NAME client_cache_test COMMAND client_cache_test)

    # Master tests
    add_executable(master_test
        tests/master/inode_tree_test.cpp
        tests/master/inode_entry_test.cpp
        tests/master/inode_store_test.cpp
        tests/master/block_master_test.cpp
        tests/master/mount_table_test.cpp
    )
    target_link_libraries(master_test PRIVATE anycache_master GTest::gtest GTest::gtest_main)
    add_test(NAME master_test COMMAND master_test)
endif()

# ─── Benchmarks ───────────────────────────────────────────────
if(ANYCACHE_BUILD_BENCHMARKS)
    include(FetchContent)
    FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(cache_benchmark benchmark/cache_benchmark.cpp)
    target_link_libraries(cache_benchmark PRIVATE anycache_worker benchmark::benchmark)
endif()

# ─── Install ──────────────────────────────────────────────────
install(TARGETS anycache-master anycache-worker anycache-cli
    RUNTIME DESTINATION bin)
