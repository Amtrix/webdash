cmake_minimum_required(VERSION 3.10)
project(webdash-heartbeat)

set (EXTERNAL_LIB_PATH "$ENV{WEBDASH}/src/lib/external")
set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_FLAGS "-msse4.2 -Wall -Wextra -O3 -g")

###### Include directories
FIND_PACKAGE(Boost 1.65 COMPONENTS filesystem REQUIRED)
include_directories(${Boost_INCLUDE_DIR})
include_directories(${EXTERNAL_LIB_PATH}/json/include)
include_directories(${EXTERNAL_LIB_PATH}/websocketpp)
include_directories($ENV{WEBDASH}/src/common)
include_directories($ENV{WEBDASH}/src/lib/_webdash/executor/include)

###### Binaries to create
add_executable(webdash-heartbeat src/main.cpp)

###### Library paths
target_link_libraries(webdash-heartbeat -L"$ENV{WEBDASH}/app-persistent/lib")

###### Libraries
target_link_libraries(webdash-heartbeat Boost::filesystem)
target_link_libraries(webdash-heartbeat webdash-executor)
