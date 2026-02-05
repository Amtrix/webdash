#pragma once

#include <webdash-config.hpp>

#include <mutex>
#include <thread>

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

    // Adds a new configuration file to the suite.
    void RegisterConfig(const WebDashConfig& config);

    // Remvoes a configuration file from the suite, if it exists.
    void UnRegisterConfig(const string& path);

    // Reload all configs to accept any changes.
    void ReloadAll();

    // Spawns a thread to periodically call ::RunOnce() over all our scripts.
    void Run();

    string GetRegisteredConfigsJsonDump();

    vector<string> GetRegisteredConfigPaths();

private:
    // Serializes configuration into a json string. Has to be understood by ::LoadSaved.
    void SerializeAndSave();

    // Called by ::Run to periodically execute all configs.
    void RunOnce();

    // JSON that was loaded from previous serializations (by SerializeAndSave).
    json _json_source_data;

    // Holds the object representatives of all script files we own in this suite.
    vector<WebDashConfig> _scripts;

    // Protects areas with parallel access.
    std::mutex _suite_mutex;

    // Thread that performs periodic running of scripts.
    std::thread _periodic_parse;

    // Signals that all resources should work towards releasing themselves when set to `false`.
    bool _is_alive;
};
