cmake_minimum_required(VERSION 3.10)

project(webdash-server)

FIND_PACKAGE(Boost 1.65 COMPONENTS system filesystem REQUIRED)
INCLUDE_DIRECTORIES( ${Boost_INCLUDE_DIR} )
INCLUDE_DIRECTORIES( "$ENV{WEBDASH}/src/common" )
INCLUDE_DIRECTORIES( "$ENV{WEBDASH}/src/bin/_webdash/server/common" )
INCLUDE_DIRECTORIES( "$ENV{WEBDASH}/src/bin/_webdash/common-internal" )

set (EXTERNAL_LIB_PATH "$ENV{WEBDASH}/src/lib/external")
set (CMAKE_CXX_COMPILER /usr/bin/g++)
set (CMAKE_CXX_FLAGS "-std=c++20 -msse4.2 -Wall -Wextra -O3 -g -fopenmp -lstdc++fs")

include_directories(${EXTERNAL_LIB_PATH}/json/include)
include_directories(${EXTERNAL_LIB_PATH}/websocketpp)
include_directories($ENV{WEBDASH}/src/lib/_webdash/executor/include)

list(APPEND ALL_CPP_FILES
    "src/WebDashCore.cpp"
    "src/Suite.cpp"
    "src/HttpFileServer.cpp"
)


add_executable(webdash-server src/main.cpp)
target_sources(webdash-server PRIVATE ${ALL_CPP_FILES})

###### Library paths
target_link_libraries(webdash-server -L"$ENV{WEBDASH}/app-persistent/lib")

###### Libraries
target_link_libraries(webdash-server Boost::filesystem)
target_link_libraries(webdash-server webdash-executor)
