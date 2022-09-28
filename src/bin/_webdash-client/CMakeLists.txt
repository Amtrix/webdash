cmake_minimum_required(VERSION 3.10)

project(webdash-client)

set (EXTERNAL_LIB_PATH "$ENV{MYWORLD}/src/lib/external")
set (CMAKE_CXX_COMPILER /usr/bin/g++-9)

# -rdynamic Keeps symbol names for readable output from the backtrace_symbols() call.
set (CMAKE_CXX_FLAGS "-std=c++1z -msse4.2 -Wall -Wextra -O3 -g -fopenmp -lstdc++fs -rdynamic")


###### Include directories
FIND_PACKAGE(Boost REQUIRED COMPONENTS system filesystem)
include_directories(${Boost_INCLUDE_DIR})
include_directories(${EXTERNAL_LIB_PATH}/json/include)
include_directories(${EXTERNAL_LIB_PATH}/websocketpp)
include_directories($ENV{MYWORLD}/src/common)
include_directories($ENV{MYWORLD}/src/lib/webdash-executor/include)
include_directories(./include)


###### *.cpp code to compile
list(APPEND ALL_CPP_FILES

)

###### Binaries to create
add_executable(webdash src/main.cpp)
target_sources(webdash PRIVATE ${ALL_CPP_FILES})

###### Library paths
target_link_libraries(webdash -L"$ENV{MYWORLD}/app-persistent/lib")


###### Libraries
target_link_libraries(webdash Boost::filesystem)
target_link_libraries(webdash webdash-executor)