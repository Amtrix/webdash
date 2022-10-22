#pragma once

#include "webdash-config-task.hpp"
#include "webdash-core.hpp"
#include "webdash-utils.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>

using namespace std;
using json = nlohmann::json;
using ConfigAndCommand = pair<WebDashConfig, string>;


/**
 * @class In memory representation of a (on-disk stored) WebDash config file.
 */
class WebDashConfig
{
public:

    /**
     * @brief Loads the given JSON file into a new WebDashConfig object.
     * @param config_filepath The path to the JSON config file.
     **/
    WebDashConfig(const std::filesystem::path config_filepath);


    /**
     * @brief Runs a single task with name @param command_name or all if none
     *        provided or "" is provided.
     * @param command_name The name of the command in the WebDash config to run.
     * @returnsA RunReturn object that stores all the output information of the
     *         process.
     */
    std::vector<WebDashType::RunReturn> Run(const string command_name = "", WebDashType::RunConfig runconfig = {});


    /**
     * @brief Returns all substitutions pairs for the WebDash config, usable for
     *        any JSON values in config files.
     * @returnsThe substitutions of pairs.
     */
    std::vector<std::pair<string,string>> GetProfileConfigSubtitutions() const;


    /**
     * @brief Reload the config file.
     */
    void Reload();


    /**
     * @brief The file path to the config's JSON.
     * @returnsThe file path.
     */
    string GetPath() const;


    /**
     * @brief The loading/reloading of the JSON config may have failed. This
     *        functions boolean return value reflects that result.
     * @returnsReturns `true` if the loading succeeded, `false` otherwise.
     */
    bool LastLodingSucceeded() const { return !_loading_failed; };


    /**
     * @returnsGet the list of task names in the JSON config.
     */
    vector<string> GetTaskList();

    /**
     * @returnsReturns a WebDashConfigTask object, given the task's name.
     */
    std::optional<WebDashConfigTask> GetTask(const string command_name);


private:

    /**
     *  @brief Returns a hollow object.
     */
    WebDashConfig() { };


    /**
     *  @brief Loads the config and returns the array of tasks the config defines.
     *
     *  @param config_filepath The path to the WebDash config file.
     *  @returnsAll tasks in the config.
     *  @throw WebDashException::ConfigJsonParseError if pasing of the JSON failed.
     */
    vector<WebDashConfigTask> Load(const std::filesystem::path config_filepath);


    /**
     *  @brief Loads the config and catches a set of JSON parsing exceptions for which it returns @retval nullopt.
     *
     *  @param config_filepath The path to the WebDash config file.
     *  @returnsAll tasks in the config or @retval nullopt if the config is an invalid JSON file.
     */
    optional<vector<WebDashConfigTask>> LoadAndCheckKnownFailures(const std::filesystem::path& config_filepath);


    // The array of tasks in the config.
    vector<WebDashConfigTask> _tasks;

    // The path to the location of the config file.
    std::filesystem::path _config_filepath;

    // If TRUE, then the loading of the config at the given path has failed (e.g., malformed JSON).
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
