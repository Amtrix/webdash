#pragma once

#include <string>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <map>
#include <mutex>
#include <optional>

#include <webdash-types.hpp>
#include <webdash-constants.hpp>

/**
 * WebDashProject - Efficient logging for WebDash projects.
 *
 * Instead of opening/closing log files on every Log() call, this class
 * keeps file handles open for the lifetime of the project, significantly
 * improving performance especially on slow filesystems (like WSL's /mnt/c/).
 *
 * Usage:
 *   // At program start
 *   WebDashProject project("my-project-name");
 *
 *   // During execution
 *   project.Log(WebDashType::LogType::INFO, "Something happened");
 *   project.Log(WebDashType::LogType::DEBUG, "Debug info");
 *
 *   // At program end (or let destructor handle it)
 *   project.End();
 *
 * The destructor will automatically call End() if not called explicitly.
 */
class WebDashProject {
public:
    /**
     * Initialize the project and open log files.
     * @param project_name Name used for log directory (app-temporary/logging/<project_name>/)
     */
    explicit WebDashProject(const std::string& project_name);

    /**
     * Destructor - calls End() if not already called.
     */
    ~WebDashProject();

    // Non-copyable
    WebDashProject(const WebDashProject&) = delete;
    WebDashProject& operator=(const WebDashProject&) = delete;

    // Movable
    WebDashProject(WebDashProject&& other) noexcept;
    WebDashProject& operator=(WebDashProject&& other) noexcept;

    /**
     * Log a message. File is already open, so this is fast.
     * @param type Log level (DEBUG, INFO, WARN, ERR)
     * @param msg The message to log
     */
    void Log(WebDashType::LogType type, const std::string& msg);

    /**
     * Flush all log buffers and close file handles.
     * Safe to call multiple times.
     */
    void End();

    /**
     * Flush log buffers without closing files.
     */
    void Flush();

    /**
     * Get the project name.
     */
    const std::string& GetName() const { return _project_name; }

    /**
     * Check if project is still active (End() not called).
     */
    bool IsActive() const { return !_ended; }

private:
    void OpenLogFiles();
    void CloseLogFiles();
    std::string GetTimestamp() const;
    void CheckAndRotate(WebDashType::LogType type);

    std::string _project_name;
    std::filesystem::path _log_directory;
    std::map<WebDashType::LogType, std::ofstream> _log_files;
    std::map<WebDashType::LogType, std::filesystem::path> _log_paths;
    bool _ended = false;
    std::mutex _mutex;  // Thread safety for logging

    // Maximum log file size before rotation (10 MB)
    static constexpr std::uintmax_t kMaxLogFileSize = 10 * 1024 * 1024;
};


/**
 * Global WebDashProject instance management.
 *
 * For compatibility with existing code that uses WebDashCore::Get().Log(),
 * we provide a global project that can be initialized once.
 */
namespace WebDashProjectGlobal {
    /**
     * Initialize the global project instance.
     * Call this at program startup.
     */
    void Init(const std::string& project_name);

    /**
     * Get the global project instance.
     * Throws if Init() was not called.
     */
    WebDashProject& Get();

    /**
     * End the global project (flush and close files).
     * Call this at program shutdown.
     */
    void End();

    /**
     * Check if global project is initialized.
     */
    bool IsInitialized();
}
