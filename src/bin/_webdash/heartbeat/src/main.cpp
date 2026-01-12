/**
 * WebDash Heartbeat
 *
 * Integration test for WebDash scheduled execution. Logs a timestamp
 * every minute to verify the scheduling system is working correctly.
 *
 * When registered with WebDash, this runs automatically on schedule
 * and writes to the WebDash logging system.
 **/

#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <webdash-core.hpp>
#include <webdash-types.hpp>

using namespace std;

const string _WEBDASH_PROJECT_NAME_ = "webdash-heartbeat";

string GetTimestamp() {
    auto now = chrono::system_clock::now();
    auto time_t_now = chrono::system_clock::to_time_t(now);
    auto ms = chrono::duration_cast<chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    stringstream ss;
    ss << put_time(localtime(&time_t_now), "%Y-%m-%d %H:%M:%S")
       << '.' << setfill('0') << setw(3) << ms.count();
    return ss.str();
}

int main() {
    string timestamp = GetTimestamp();
    string message = "Heartbeat: " + timestamp;

    // Log to WebDash logging system
    WebDash().Log(WebDashType::LogType::INFO, message);

    // Also print to stdout for manual verification
    cout << message << endl;

    return 0;
}
