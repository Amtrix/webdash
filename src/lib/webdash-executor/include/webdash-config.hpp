#pragma once

#include "webdash-config-task.hpp"
#include "webdash-core.hpp"
#include "webdash-utils.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>

using namespace std;
using json = nlohmann::json;
using ConfigAndCommand = pair<WebDashConfig, string>;

class WebDashConfig
{
public:

    enum class LoadFailureReason
    {
        NoError,
        FileNotFound
    };

    WebDashConfig(const std::filesystem::path config_filepath);

    /**
     * Runs a single task with name {cmdName} or all if none provided or "" is
     * provided.
     */
    std::vector<WebDashType::RunReturn> Run(const string cmdName = "", WebDashType::RunConfig runconfig = {});

    std::vector<std::pair<string,string>> GetProfileConfigSubtitutions() const;

    void Reload();

    string GetPath() const;

    bool LastLodingSucceeded() const { return !_loading_failed; };

    vector<string> GetTaskList();

    std::optional<WebDashConfigTask> GetTask(const string cmdname);

private:

    WebDashConfig() { };

    /**
     *  @brief Loads the config. Returns FALSE upon detected failure.
     *  @param config_filepath The path to the WebDash config file.
     *  @return All tasks in the config
     *  @throw WebDashException::ConfigJsonParseError if pasing of the JSON
     *         failed.
     */
    vector<WebDashConfigTask> Load(const std::filesystem::path config_filepath);

    optional<vector<WebDashConfigTask>> LoadAndCheckKnownFailures(const std::filesystem::path path);

    vector<WebDashConfigTask> _tasks;

    // The path to the location of the config file.
    std::filesystem::path _path;

    // If the loading failed, MAY contain the reason for the failure.
    LoadFailureReason load_failure_reason;

    // If TRUE, then the loading of the config at the given path has failed.
    // This is most often due to the webdash config JSON being badly
    // formatted.
    bool _loading_failed;
};


/**
 * @brief Given a path to a file OR directory, identifies the "best" matching
 *        WebDash configuration file:
 *
 *        1) If the file is not a directory, consider it as the configuration
 *           file.
 *
 *        2) If the file is a directory, check it for the `webdash.config.json`
 *           file. If `check_ancestry` is `true`, also check all directories in
 *           the ancestry chain.
 *
 * @param path The path to the file or directory for which to identify the best
 *             matching configuration file.
 * @param check_ancestry If set, will also look through the ancestor directories
 *                       for a configuration file.
 * @returns The best matching configuration file; nullopt if none could be
 *          determined.
 */
inline std::optional<WebDashConfig> GetBestMatchingConfig(const std::filesystem::path& path,
                                                          const bool check_ancestry = true);


/**
 * @brief Given the arguments (generally, passed through the command line),
 *        attempts to identify a WebDash configuration file and a matching
 *        command to execute.
 * @param arguments The arguments to parse.
 * @returns A pair { config, command } if identification was possibe; `nullopt`
 *          otherwise.
 */
std::optional<ConfigAndCommand> GetConfigAndCommand(const vector<string>& arguments);
