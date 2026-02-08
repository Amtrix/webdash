cmake_minimum_required(VERSION 3.30)

project(webdash-client LANGUAGES CXX)

set(EXTERNAL_LIB_PATH "$ENV{WEBDASH}/src/lib/external")

# CMake >= 3.30: FindBoost is removed; use legacy behavior explicitly (or provide BoostConfig.cmake).
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

find_package(Boost REQUIRED COMPONENTS system filesystem)

add_executable(webdash)

target_sources(webdash PRIVATE
  src/main.cpp
  # add other .cpp files here
)

target_include_directories(webdash
  PRIVATE
    "${EXTERNAL_LIB_PATH}/json/include"
    "${EXTERNAL_LIB_PATH}/websocketpp"
    "$ENV{WEBDASH}/src/common"
    "$ENV{WEBDASH}/src/lib/_webdash/executor/include"
    "$ENV{WEBDASH}/src/bin/_webdash/common-internal"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(webdash PRIVATE cxx_std_20)

# Move flags from global CMAKE_CXX_FLAGS into target-scoped options
target_compile_options(webdash PRIVATE -msse4.2 -Wall -Wextra -O3 -g -fopenmp)
target_link_options(webdash PRIVATE -rdynamic)

# Do not use -lstdc++fs for C++20; remove it.
# Do not set CMAKE_CXX_COMPILER in CMakeLists.txt; pass -DCMAKE_CXX_COMPILER=... when configuring.

# Library search path and runtime path
target_link_directories(webdash PRIVATE "$ENV{WEBDASH}/app-persistent/lib")
set_target_properties(webdash PROPERTIES
  BUILD_RPATH "$ENV{WEBDASH}/app-persistent/lib"
  INSTALL_RPATH "$ENV{WEBDASH}/app-persistent/lib"
)

target_link_libraries(webdash
  PRIVATE
    Boost::filesystem
    Boost::system
    webdash-executor
)
