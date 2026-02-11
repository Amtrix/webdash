#pragma once

#include <webdash-config.hpp>

#include <mutex>
#include <shared_mutex>
#include <thread>
#include <optional>
#include <deque>
#include <chrono>
#include <filesystem>
#include <sys/types.h>

/**
 * Registration info for a config - includes the config and optional user context.
 */
struct RegisteredConfig {
    WebDashConfig config;
    std::optional<uid_t> run_as_uid;
    std::optional<gid_t> run_as_gid;
    std::optional<std::string> run_as_home;
    std::optional<std::filesystem::file_time_type> last_write_time;
    std::optional<std::string> project_name;
    std::optional<int> priority;

    RegisteredConfig(const WebDashConfig& c) : config(c) {}
    RegisteredConfig(const WebDashConfig& c, uid_t uid, gid_t gid, const std::string& home)
        : config(c), run_as_uid(uid), run_as_gid(gid), run_as_home(home) {}
};

/**
 * A single recorded action that the server executed (or attempted).
 */
struct ActionLogEntry {
    std::string timestamp;      ///< ISO-8601 local time.
    std::string config_path;    ///< Path of the webdash.config.json.
    std::string task_name;      ///< Name of the task that ran.
    int         return_code;    ///< Exit code (0 = success).
};

/**
 * Represents a set of multiple config files. Manages storage of its configuration. Periodically executes
 * its stored configurations.
 * */
class Suite {
public:
    Suite();

    // We have to perform a custom deconstruct as some resources require special attention for clean exit.
    ~Suite();

    // Loads the previously serialized and saved state.
    void LoadSaved();

    // Adds a new configuration file to the suite (runs as root).
    void RegisterConfig(const WebDashConfig& config);

    // Adds a new configuration file to the suite with user context.
    void RegisterConfig(const WebDashConfig& config, uid_t uid, gid_t gid, const std::string& home);

    // Remvoes a configuration file from the suite, if it exists.
    void UnRegisterConfig(const string& path);

    // Reload all configs to accept any changes.
    void ReloadAll();

    // Spawns a thread to periodically call ::RunOnce() over all our scripts.
    void Run();

    string GetRegisteredConfigsJsonDump();

    vector<string> GetRegisteredConfigPaths();

    // Get user context for a registered config path
    std::optional<RegisteredConfig> GetRegisteredConfig(const string& path);

    // Get all network-shared project directories.
    // Returns pairs of (project_name, directory_path) for configs with network-shared: true.
    vector<pair<string, string>> GetNetworkSharedPaths();

    // Returns the most recent N action log entries (newest first).
    vector<ActionLogEntry> GetRecentActions(size_t max_count = 20);

    // Run a specific task by config path and task name (async, in a detached thread).
    // Returns true if the config was found and the task was dispatched.
    bool RunTask(const string& config_path, const string& task_name);

    /// Set a callback that fires whenever the action log or config list changes.
    /// Used to push WebSocket updates to connected portal clients.
    void SetOnChangeCallback(std::function<void()> cb) { _on_change = std::move(cb); }

private:
    // Serializes configuration into a json string. Has to be understood by ::LoadSaved.
    void SerializeAndSave();

    // Called by ::Run to periodically execute all configs.
    void RunOnce();

    // Record an action into the log (thread-safe, called from RunOnce).
    void RecordAction(const std::string& config_path, const std::string& task_name, int return_code);

    // JSON that was loaded from previous serializations (by SerializeAndSave).
    json _json_source_data;

    // Holds registered configs with their user context.
    vector<RegisteredConfig> _scripts;

    // Protects areas with parallel access.
    std::shared_mutex _suite_mutex;

    // Protects the action log.
    std::mutex _action_log_mutex;

    // Capped action log (newest at back).
    std::deque<ActionLogEntry> _action_log;
    static constexpr size_t kMaxActionLogSize = 200;

    // Thread that performs periodic running of scripts.
    std::thread _periodic_parse;

    // Signals that all resources should work towards releasing themselves when set to `false`.
    bool _is_alive;

    // Optional callback to notify listeners of state changes (e.g. WebSocket broadcast).
    std::function<void()> _on_change;
    void NotifyChange() { if (_on_change) _on_change(); }
};
