/**
 * WebDash logging test - C++
 * Tests the webdash C++ logging API.
 *
 * Compile with:
 *   g++ -std=c++20 -I$WEBDASH/src/lib/_webdash/executor/include \
 *       -I$WEBDASH/src/lib/external/json/include \
 *       -L$WEBDASH/app-persistent/lib \
 *       -o webdash-logtests-cpp webdash-logtests-cpp.cpp \
 *       -lwebdash-executor -lboost_filesystem
 */

#include <iostream>
#include <string>

#include <webdash-project.hpp>
#include <webdash-types.hpp>

// Still needed for backward compatibility with executor library
#include <webdash-core.hpp>
using std::string;
const string _WEBDASH_PROJECT_NAME_ = "webdash-logtests";

int main() {
    const std::string FILENAME = "webdash-logtests-cpp.cpp";

    // Create a project - opens log files once
    WebDashProject project("webdash-logtests");

    // Log at all levels - files already open, so this is fast!
    project.Log(WebDashType::LogType::DEBUG, "DEBUG log from " + FILENAME);
    project.Log(WebDashType::LogType::INFO, "INFO log from " + FILENAME);
    project.Log(WebDashType::LogType::WARN, "WARN log from " + FILENAME);
    project.Log(WebDashType::LogType::ERR, "ERROR log from " + FILENAME);

    // Destructor calls End() automatically, flushing and closing files
    std::cout << "C++ logging test complete - logged to webdash-logtests" << std::endl;

    return 0;
}
