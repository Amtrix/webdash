cmake_minimum_required(VERSION 3.10)

project(WebDashLibrary)

FIND_PACKAGE(Boost COMPONENTS system filesystem REQUIRED)
INCLUDE_DIRECTORIES( ${Boost_INCLUDE_DIR} )
INCLUDE_DIRECTORIES( "$ENV{MYWORLD}/src/common" )
INCLUDE_DIRECTORIES( "$ENV{MYWORLD}/src/bin/_webdash/common" )
INCLUDE_DIRECTORIES( "${CMAKE_CURRENT_SOURCE_DIR}/include" )

set (CMAKE_ARCHIVE_OUTPUT_DIRECTORY "$ENV{MYWORLD}/app-persistent/lib")
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY "$ENV{MYWORLD}/app-persistent/lib")
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY "$ENV{MYWORLD}/app-persistent/lib")

set (EXTERNAL_LIB_PATH "$ENV{MYWORLD}/src/lib/external")
set (CMAKE_CXX_COMPILER /usr/bin/g++)

# -rdynamic Keeps symbol names for readable output from the backtrace_symbols() call.
set (CMAKE_CXX_FLAGS "-std=c++20 -msse4.2 -Wall -Wextra -O3 -g -fopenmp -lstdc++fs -rdynamic")

include_directories(${EXTERNAL_LIB_PATH}/json/include)
include_directories(${EXTERNAL_LIB_PATH}/websocketpp)

list(APPEND ALL_CPP_FILES
    "src/webdash-config.cpp"
    "src/webdash-config-task.cpp"
    "src/webdash-core.cpp"
    "src/webdash-utils.cpp"
)

# Macros.
add_compile_definitions(_PLATFORM_LINUX)

# Adds a library target called <name> to be built from the source files listed
# in the command invocation
add_library(webdash-executor STATIC ${ALL_CPP_FILES} )

# Library is added, link libraries to be built with it.
target_link_libraries(webdash-executor Boost::filesystem)

