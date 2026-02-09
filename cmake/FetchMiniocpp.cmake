# FetchMiniocpp.cmake
# -------------------
# Pull minio-cpp and ALL its dependencies via FetchContent.
# Called from the top-level CMakeLists.txt when ANYCACHE_ENABLE_S3 is ON.
#
# minio-cpp (v0.3.0) depends on:
#   - OpenSSL       (system, find_package)
#   - ZLIB          (system, find_package)
#   - CURL          (system, find_package)
#   - curlpp        (FetchContent)
#   - inih          (FetchContent)
#   - nlohmann_json (FetchContent)
#   - pugixml       (FetchContent)

include(FetchContent)

# ─── System dependencies ─────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)

# ─── 1. nlohmann_json ────────────────────────────────────────
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ─── 2. pugixml ──────────────────────────────────────────────
find_package(pugixml QUIET)
if(NOT pugixml_FOUND)
    FetchContent_Declare(
        pugixml
        GIT_REPOSITORY https://github.com/zeux/pugixml.git
        GIT_TAG v1.15
    )
    FetchContent_MakeAvailable(pugixml)
endif()

# ─── 3. curlpp ───────────────────────────────────────────────
# minio-cpp expects unofficial::curlpp::curlpp target.
# curlpp depends on CURL, which we already found above.
find_package(unofficial-curlpp QUIET)
if(NOT unofficial-curlpp_FOUND)
    FetchContent_Declare(
        curlpp
        GIT_REPOSITORY https://github.com/jpbarrette/curlpp.git
        GIT_TAG 8810334c830faa3b38bcd94f5b1ab695a4f05eb9
    )
    set(CURLPP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(curlpp)

    # curlpp's CMakeLists uses include_directories() instead of
    # target_include_directories(... PUBLIC ...), so the include path
    # does not propagate to dependents. Fix that here.
    if(TARGET curlpp_static)
        target_include_directories(curlpp_static PUBLIC
            $<BUILD_INTERFACE:${curlpp_SOURCE_DIR}/include>
        )
        # Create the alias that minio-cpp looks for
        if(NOT TARGET unofficial::curlpp::curlpp)
            add_library(unofficial::curlpp::curlpp ALIAS curlpp_static)
        endif()
    endif()
endif()

# ─── 4. inih (INI parser) ────────────────────────────────────
# minio-cpp expects unofficial::inih::inireader target.
# Use the OSSystems fork which has CMake support.
if(NOT TARGET unofficial::inih::inireader)
    FetchContent_Declare(
        inih
        GIT_REPOSITORY https://github.com/OSSystems/inih.git
        GIT_TAG master
        PATCH_COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR=<SOURCE_DIR>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/inih-patch.cmake
    )
    set(BUILD_SHARED_LIBS_OLD ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(inih)
    set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_OLD} CACHE BOOL "" FORCE)

    # inih's CMakeLists uses include_directories() instead of
    # target_include_directories(... PUBLIC ...), fix propagation.
    if(TARGET inih)
        target_include_directories(inih PUBLIC
            $<BUILD_INTERFACE:${inih_SOURCE_DIR}/include>
        )
    endif()
    if(TARGET inihcpp)
        target_include_directories(inihcpp PUBLIC
            $<BUILD_INTERFACE:${inih_SOURCE_DIR}/include>
        )
    endif()

    # Create alias targets that minio-cpp expects
    if(TARGET inih AND NOT TARGET unofficial::inih::libinih)
        add_library(unofficial::inih::libinih ALIAS inih)
    endif()
    if(TARGET inihcpp AND NOT TARGET unofficial::inih::inireader)
        add_library(unofficial::inih::inireader ALIAS inihcpp)
    endif()
endif()

# ─── 5. minio-cpp ────────────────────────────────────────────
FetchContent_Declare(
    minio-cpp
    GIT_REPOSITORY https://github.com/minio/minio-cpp.git
    GIT_TAG v0.3.0
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=<SOURCE_DIR>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/miniocpp-patch.cmake
)
set(MINIO_CPP_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(minio-cpp)

message(STATUS "minio-cpp integrated via FetchContent")
