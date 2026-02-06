#include "webdash-project.hpp"
#include "webdash-core.hpp"

#include <stdexcept>
#include <cstdlib>
#include <iostream>

using namespace std;

// ============================================================================
// WebDashProject implementation
// ============================================================================

WebDashProject::WebDashProject(const std::string& project_name)
    : _project_name(project_name), _ended(false) {
    OpenLogFiles();
}

WebDashProject::~WebDashProject() {
    if (!_ended) {
        End();
    }
}

WebDashProject::WebDashProject(WebDashProject&& other) noexcept
    : _project_name(std::move(other._project_name))
    , _log_directory(std::move(other._log_directory))
    , _log_files(std::move(other._log_files))
    , _log_paths(std::move(other._log_paths))
    , _ended(other._ended) {
    other._ended = true;  // Prevent other from closing files
}

WebDashProject& WebDashProject::operator=(WebDashProject&& other) noexcept {
    if (this != &other) {
        End();  // Close our files first
        _project_name = std::move(other._project_name);
        _log_directory = std::move(other._log_directory);
        _log_files = std::move(other._log_files);
        _log_paths = std::move(other._log_paths);
        _ended = other._ended;
        other._ended = true;
    }
    return *this;
}

void WebDashProject::OpenLogFiles() {
    // Determine log directory
    // Try to use WebDashCore if available, otherwise use WEBDASH env var
    const char* env_root = getenv(WebDashConst::Public::kEnvVarName);

    if (env_root != nullptr) {
        _log_directory = filesystem::path(env_root)
                        / WebDashConst::Public::kTemporaryStorageDir
                        / "logging"
                        / _project_name;
    } else {
        // Fallback: use current directory
        _log_directory = filesystem::current_path() / "logs" / _project_name;
    }

    // Create directory if it doesn't exist
    filesystem::create_directories(_log_directory);

    // Open all log files
    const std::vector<WebDashType::LogType> log_types = {
        WebDashType::LogType::DEBUG,
        WebDashType::LogType::INFO,
        WebDashType::LogType::WARN,
        WebDashType::LogType::ERR
    };

    for (auto type : log_types) {
        auto filename = string(WebDashConst::Public::kLogFilenamePrefix)
                       + WebDashType::kLogTypeToString.at(type)
                       + ".txt";
        auto path = _log_directory / filename;
        _log_paths[type] = path;

        // Check for rotation before opening
        CheckAndRotate(type);

        // Open in append mode
        _log_files[type].open(path, std::ios::out | std::ios::app);

        if (!_log_files[type].is_open()) {
            std::cerr << "WebDashProject: Failed to open log file: " << path << std::endl;
        }
    }
}

void WebDashProject::CloseLogFiles() {
    for (auto& [type, file] : _log_files) {
        if (file.is_open()) {
            file.flush();
            file.close();
        }
    }
    _log_files.clear();
}

std::string WebDashProject::GetTimestamp() const {
    auto now_c = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now_c);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_t), "%F %T");
    return ss.str();
}

void WebDashProject::CheckAndRotate(WebDashType::LogType type) {
    auto& path = _log_paths[type];

    if (!filesystem::exists(path)) {
        return;
    }

    std::error_code ec;
    auto file_size = filesystem::file_size(path, ec);

    if (ec || file_size < kMaxLogFileSize) {
        return;
    }

    // Close file if open
    if (_log_files[type].is_open()) {
        _log_files[type].close();
    }

    // Rotate: rename to previous.<filename>
    auto previous_path = path.parent_path()
                        / (string(WebDashConst::Public::kLogRotationPrefix) + path.filename().string());

    filesystem::remove(previous_path, ec);
    filesystem::rename(path, previous_path, ec);

    // Reopen fresh file
    _log_files[type].open(path, std::ios::out | std::ios::app);
    if (_log_files[type].is_open()) {
        _log_files[type] << GetTimestamp() << ": Log rotated (previous file exceeded "
                         << (kMaxLogFileSize / (1024 * 1024)) << " MB)." << std::endl;
    }
}

void WebDashProject::Log(WebDashType::LogType type, const std::string& msg) {
    if (_ended) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _log_files.find(type);
    if (it == _log_files.end() || !it->second.is_open()) {
        return;
    }

    it->second << GetTimestamp() << ": " << msg << std::endl;

    // Periodic rotation check (every 1000 writes would be better, but for simplicity just check size occasionally)
    // We skip this for performance - rotation happens at startup
}

void WebDashProject::End() {
    if (_ended) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    CloseLogFiles();
    _ended = true;
}

void WebDashProject::Flush() {
    if (_ended) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& [type, file] : _log_files) {
        if (file.is_open()) {
            file.flush();
        }
    }
}


// ============================================================================
// Global instance management
// ============================================================================

namespace {
    std::unique_ptr<WebDashProject> g_project;
    std::mutex g_project_mutex;
}

namespace WebDashProjectGlobal {

void Init(const std::string& project_name) {
    std::lock_guard<std::mutex> lock(g_project_mutex);
    if (g_project) {
        g_project->End();
    }
    g_project = std::make_unique<WebDashProject>(project_name);
}

WebDashProject& Get() {
    std::lock_guard<std::mutex> lock(g_project_mutex);
    if (!g_project) {
        throw std::runtime_error("WebDashProjectGlobal::Get() called before Init()");
    }
    return *g_project;
}

void End() {
    std::lock_guard<std::mutex> lock(g_project_mutex);
    if (g_project) {
        g_project->End();
        g_project.reset();
    }
}

bool IsInitialized() {
    std::lock_guard<std::mutex> lock(g_project_mutex);
    return g_project != nullptr && g_project->IsActive();
}

}  // namespace WebDashProjectGlobal
