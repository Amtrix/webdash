#pragma once

#include <webdash-config.hpp>

#include <mutex>
#include <thread>
#include <optional>
#include <sys/types.h>

/**
 * Registration info for a config - includes the config and optional user context.
 */
struct RegisteredConfig {
    WebDashConfig config;
    std::optional<uid_t> run_as_uid;
    std::optional<gid_t> run_as_gid;
    std::optional<std::string> run_as_home;

    RegisteredConfig(const WebDashConfig& c) : config(c) {}
    RegisteredConfig(const WebDashConfig& c, uid_t uid, gid_t gid, const std::string& home)
        : config(c), run_as_uid(uid), run_as_gid(gid), run_as_home(home) {}
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

private:
    // Serializes configuration into a json string. Has to be understood by ::LoadSaved.
    void SerializeAndSave();

    // Called by ::Run to periodically execute all configs.
    void RunOnce();

    // JSON that was loaded from previous serializations (by SerializeAndSave).
    json _json_source_data;

    // Holds registered configs with their user context.
    vector<RegisteredConfig> _scripts;

    // Protects areas with parallel access.
    std::mutex _suite_mutex;

    // Thread that performs periodic running of scripts.
    std::thread _periodic_parse;

    // Signals that all resources should work towards releasing themselves when set to `false`.
    bool _is_alive;
};
