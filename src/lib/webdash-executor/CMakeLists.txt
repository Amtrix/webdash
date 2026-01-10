cmake_minimum_required(VERSION 3.30)

project(WebDashLibrary LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

# Prefer config packages (BoostConfig.cmake); required for CMake >= 3.30.
find_package(Boost REQUIRED COMPONENTS system filesystem)

add_library(webdash-executor STATIC)

target_sources(webdash-executor PRIVATE
  src/webdash-config.cpp
  src/webdash-config-task.cpp
  src/webdash-core.cpp
  src/webdash-utils.cpp
)

# Output dirs
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "$ENV{WEBDASH}/app-persistent/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "$ENV{WEBDASH}/app-persistent/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "$ENV{WEBDASH}/app-persistent/lib")

set(EXTERNAL_LIB_PATH "$ENV{WEBDASH}/src/lib/external")

# Include paths (target-scoped)
target_include_directories(webdash-executor
  PRIVATE
    "$ENV{WEBDASH}/src/common"
    "$ENV{WEBDASH}/src/bin/_webdash/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${EXTERNAL_LIB_PATH}/json/include"
    "${EXTERNAL_LIB_PATH}/websocketpp"
)

# Compile definitions (target-scoped)
target_compile_definitions(webdash-executor PRIVATE _PLATFORM_LINUX)

# C++ standard (target-scoped)
target_compile_features(webdash-executor PRIVATE cxx_std_20)

# Compile/link options (target-scoped)
target_compile_options(webdash-executor PRIVATE -msse4.2 -Wall -Wextra -O3 -g -fopenmp)
target_link_options(webdash-executor PRIVATE -rdynamic)

# Link Boost targets (filesystem depends on system on some platforms)
target_link_libraries(webdash-executor PRIVATE Boost::filesystem Boost::system)

# NOTE:
# - Removed INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIR}) and Boost_INCLUDE_DIR usage.
# - Removed "-lstdc++fs": for C++20 it is unnecessary and can break link lines.
# - Removed setting CMAKE_CXX_COMPILER: pass -DCMAKE_CXX_COMPILER=... on the cmake command line instead.
