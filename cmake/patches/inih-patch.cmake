# inih-patch.cmake
# Fix cmake_minimum_required(VERSION 2.6) -> 3.10 for modern CMake compatibility
# SRC_DIR is passed via -DSRC_DIR=<SOURCE_DIR>

file(READ "${SRC_DIR}/CMakeLists.txt" content)
string(REPLACE
    "cmake_minimum_required(VERSION 2.6)"
    "cmake_minimum_required(VERSION 3.10)"
    content "${content}")
file(WRITE "${SRC_DIR}/CMakeLists.txt" "${content}")
