#include "webdash-utils.hpp"
#include "webdash-config.hpp"
#include "webdash-types.hpp"
#include "webdash-core.hpp"
#include "webdash-constants.hpp"

#include <iostream>
#include <fstream>
#include <optional>
#include <cstdlib>
#include <set>
#include <nlohmann/json.hpp>
using namespace std;


namespace {

    /**
     * @brief Normalize a path by removing trailing slashes for comparison.
     */
    string NormalizePath(const string& path) {
        string result = path;
        while (!result.empty() && (result.back() == '/' || result.back() == '\\')) {
            result.pop_back();
        }
        return result;
    }

    /**
     * @brief Check if path_a is a prefix of or equal to path_b (for module matching).
     *        Handles paths like "/foo/bar" matching "/foo/bar/webdash.config.json"
     */
    bool PathMatchesModule(const string& module_path, const string& dependency_path) {
        string norm_module = NormalizePath(module_path);
        string norm_dep = NormalizePath(dependency_path);

        // Exact match
        if (norm_module == norm_dep) return true;

        // Module path is a prefix of dependency path
        if (norm_dep.find(norm_module) == 0) {
            // Make sure it's a directory boundary
            if (norm_dep.size() > norm_module.size() &&
                (norm_dep[norm_module.size()] == '/' || norm_dep[norm_module.size()] == '\\')) {
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Prompt the user for confirmation.
     * @param message The message to display.
     * @returns true if user confirms (y/Y/yes), false otherwise.
     */
    bool PromptUserConfirmation(const string& message) {
        std::cout << message << " [y/N]: ";
        std::cout.flush();

        string response;
        std::getline(std::cin, response);

        // Trim whitespace
        while (!response.empty() && std::isspace(response.front())) response.erase(0, 1);
        while (!response.empty() && std::isspace(response.back())) response.pop_back();

        // Convert to lowercase for comparison
        for (char& c : response) c = std::tolower(c);

        return response == "y" || response == "yes";
    }

    /**
     * @brief Try to auto-clone a module if the path doesn't exist.
     *        Searches the "modules" array in webdash-profile.json for a matching path.
     * @param path The path that doesn't exist.
     * @returns true if successfully cloned, false otherwise.
     */
    bool TryAutoCloneModule(const std::filesystem::path& path) {
        // Check if the path already has valid content:
        // - If it's a file, it exists
        // - If it's a directory, it must contain webdash.config.json
        if (std::filesystem::exists(path)) {
            if (std::filesystem::is_directory(path)) {
                // Directory exists - check if it has webdash.config.json
                auto config_path = path / WebDashConst::Public::kProjectConfigFilename;
                if (std::filesystem::exists(config_path)) {
                    return true;  // Already cloned and has config
                }
                // Directory exists but is empty/incomplete - continue to try cloning
            } else {
                return true;  // It's a file and exists
            }
        }

        string path_str = path.string();

        // Get modules from profile
        auto modules = WebDashCore::Get().GetExternalGitProjects();

        for (const auto& module : modules) {
            if (module.destination.empty() || module.source.empty()) {
                continue;
            }

            if (PathMatchesModule(module.destination, path_str)) {
                // Check if the module destination already has a valid webdash.config.json
                // If so, don't try to clone - the module is already set up, we just need
                // to access a subdirectory that may not exist yet
                std::filesystem::path dest_config = std::filesystem::path(module.destination) / WebDashConst::Public::kProjectConfigFilename;
                if (std::filesystem::exists(dest_config)) {
                    WebDash().Log(WebDashType::LogType::DEBUG,
                        "Module already has valid config, skipping clone: " + module.destination);
                    return true;  // Module exists, subdirectory access is up to caller
                }

                // Check if interactive clone is enabled
                if (WebDashCore::Get().IsInteractiveCloneEnabled()) {
                    string prompt = "Module not found: " + module.destination + "\n"
                                  + "Clone from " + module.source + "?";
                    if (!PromptUserConfirmation(prompt)) {
                        WebDash().Log(WebDashType::LogType::INFO,
                            "User declined to clone module: " + module.source);
                        return false;
                    }
                }

                WebDash().Log(WebDashType::LogType::INFO,
                    "Auto-cloning module: " + module.source + " -> " + module.destination);

                // If destination directory exists but is empty/incomplete, remove it first
                // (git clone fails if the directory exists)
                if (std::filesystem::exists(module.destination) &&
                    std::filesystem::is_directory(module.destination)) {
                    WebDash().Log(WebDashType::LogType::DEBUG,
                        "Removing incomplete directory: " + module.destination);
                    std::filesystem::remove_all(module.destination);
                }

                // Build git clone command
                string clone_cmd = "git clone";
                if (!module.branch.empty()) {
                    clone_cmd += " -b " + module.branch;
                }
                clone_cmd += " \"" + module.source + "\" \"" + module.destination + "\"";

                WebDash().Log(WebDashType::LogType::DEBUG, "Running: " + clone_cmd);

                auto result = WebDashUtils::ExecuteCommand(clone_cmd);

                if (result.Failed()) {
                    WebDash().Log(WebDashType::LogType::ERR,
                        "Failed to clone module: " + module.source + " -> " + module.destination);
                    if (!result.output.empty()) {
                        WebDash().Log(WebDashType::LogType::ERR, "Clone output: " + result.output);
                    }
                    return false;
                }

                WebDash().Log(WebDashType::LogType::INFO,
                    "Successfully cloned: " + module.destination);

                // Run post-clone task if specified
                if (!module.webdash_task.empty()) {
                    WebDash().Log(WebDashType::LogType::INFO,
                        "Running post-clone task: " + module.webdash_task);
                    // This will be handled by the normal dependency resolution
                }

                return true;
            }
        }

        return false;
    }

    /**
     * @brief Used for WebDash command-line parsing. Given a string, returns the prefix until the first
     *        colon character (':') or the whole string if no colon exists.
     * @param arg The string to parse.
     * @returns The prefix until the first colon character or the whole string if no colon exists.
     */

    /**
     * @brief Used for WebDash command-line parsing. Given a string <A>:<B>, where <A> and <B> are
     *        strings, with <A> being of non-zero length, the function returns the content of <A>.
     * @param arg The string to parse.
     * @returns The string content after the colon; or `nullopt` if no colon exists or the content is
     *          empty.
     */
    std::optional<string> ParseArgumentForPathWithCommandPrecedence(const string& arg) {
        auto it = arg.find(":");

        if (it == string::npos || it == 0) return nullopt;
        else return arg.substr(0, it);
    }

    /**
     * @brief Used for WebDash command-line parsing. Given a string <A>:<B>, where <A> and <B> are
     *        strings, with <B> being of non-zero length, the function returns the content of <B>.
     * @param arg The string to parse.
     * @returns The string content after the colon; or `nullopt` if no colon exists or the content is
     *          empty.
     */
    std::optional<string> ParseArgumentForCommandWithPathPrecedence(const string& arg) {
        auto it = arg.find(":");

        if (it == string::npos || it == arg.size() - 1) return nullopt;
        else return arg.substr(it + 1, arg.size() - it);
    }

} // namespace


WebDashConfig::WebDashConfig(const std::filesystem::path config_filepath) {
    // Check if the file exists before trying to canonicalize
    if (!std::filesystem::exists(config_filepath)) {
        _loading_failed = true;
        _config_filepath = config_filepath;
        return;
    }

    auto canonical_config_filepath = std::filesystem::canonical(config_filepath);

    const auto& previous_config_filepath = _config_filepath;
    _config_filepath = canonical_config_filepath;

    auto computed_tasks = LoadAndCheckKnownFailures(canonical_config_filepath);

    if (computed_tasks) {
        _loading_failed = false;
        _tasks = computed_tasks.value();
    } else {
        _loading_failed = true;
        _config_filepath = previous_config_filepath;
    }
}


namespace {
    // Track which config files have already reported parse errors to avoid duplicate messages
    std::set<std::string> reported_parse_errors;
}

std::optional<vector<WebDashConfigTask>> WebDashConfig::LoadAndCheckKnownFailures(
    const std::filesystem::path& config_filepath)
{
    try {
        return Load(config_filepath);
    }
    catch (const WebDashException::ConfigJsonParseError& e) {
        _loading_error = e.what();
        // Only report each file's error once
        std::string path_str = config_filepath.string();
        if (reported_parse_errors.find(path_str) == reported_parse_errors.end()) {
            reported_parse_errors.insert(path_str);
            WebDash().Log(WebDashType::LogType::ERR, "Failed to parse config '" + path_str + "': " + e.what());
            std::cerr << "ERROR: Failed to parse '" << path_str << "': " << e.what() << std::endl;
        }
        return nullopt;
    }

    return nullopt;
}


vector<WebDashConfigTask> WebDashConfig::Load(const std::filesystem::path config_filepath) {
    vector<WebDashConfigTask> tasks;

    ifstream configStream;
    try {
        configStream.open(config_filepath.c_str(), ifstream::in);
    }
    catch (...) {
        throw WebDashException::ConfigJsonParseError("Failed to open the config file.");
    }

    json json_config;
    try {
        configStream >> json_config;
    } catch (...) {
        throw WebDashException::ConfigJsonParseError("Unable to parse as JSON. Format error?");
    }

    json json_commands;
    try {
        json_commands = json_config["commands"];
    } catch (...) {
        throw WebDashException::ConfigJsonParseError("Failed to parse the 'commands' key. Exists?");
    }

    WebDash().Log(WebDashType::LogType::DEBUG, "Commands loaded. Available count: " + to_string(json_commands.size()));

    // Parse top-level dependencies (applies to all commands in this config)
    json top_level_deps;
    try {
        top_level_deps = json_config["dependencies"];
    } catch (...) {
        // No top-level dependencies, that's fine
    }

    int command_index = 0;

    for (auto json_command : json_commands) {
        try {
            // Merge top-level dependencies into command if they exist
            if (!top_level_deps.is_null() && top_level_deps.is_array()) {
                if (!json_command.contains("dependencies")) {
                    json_command["dependencies"] = json::array();
                }
                for (const auto& dep : top_level_deps) {
                    json_command["dependencies"].push_back(dep);
                }
            }

            const string command_identifier = config_filepath.string() + "#" + json_command["name"].get<std::string>();
            tasks.emplace_back(this, command_identifier, json_command);
        } catch (...) {
            WebDash().Log(WebDashType::LogType::DEBUG, "Failed getting name from " + to_string(command_index) + "th command. Ignored.");
        }

        command_index++;
    }

    return tasks;
}


string WebDashConfig::GetPath() const {
    return _config_filepath;
}


vector<SubstitutionPair> WebDashConfig::GetProfileConfigSubtitutions() const {
    vector<SubstitutionPair> config_substitutions;

    const auto& keychain_values = WebDashCore::Get().GetKeyValuesFromRootProfile();

    for (auto& keychain_value : keychain_values) {
        config_substitutions.push_back(WebDashUtils::GenerateWebDashConfigSubstitution(keychain_value));
    }

    // Primary substitutions, applicable to all WebDash configs.
    vector<SubstitutionPair> primary_substitutions = WebDashCore::Get().GetPrimaryKeywordSubstitutions();
    config_substitutions.insert(config_substitutions.end(), primary_substitutions.begin(), primary_substitutions.end());

    // Substitutions specific to this config file.
    config_substitutions.push_back(make_pair(WebDashConst::Public::kSubstThisDir, WebDashUtils::GetDirectoryOfFilepath(_config_filepath)));

    return config_substitutions;
}


void WebDashConfig::Reload() {
    auto computed_tasks = LoadAndCheckKnownFailures(_config_filepath);

    if (computed_tasks) {
        _loading_failed = false;
        _tasks = computed_tasks.value();
    } else {
        _loading_failed = true;
    }
}


vector<string> WebDashConfig::GetTaskList() {
    vector<string> ret;

    for (auto task : _tasks) {
        ret.push_back(task.GetName());
    }

    return ret;
}


std::optional<WebDashConfigTask> WebDashConfig::GetTask(const string command_name) {
    for (auto task : _tasks) {
        if (task.GetName() == command_name) {
            return task;
        } else if (command_name == "" && task.GetName() == WebDashConst::Public::kDefaultTaskName) {
            return task;
        }
    }

    return nullopt;
}


std::vector<WebDashType::RunReturn> WebDashConfig::Run(const string command_name, WebDashType::RunConfig runconfig, const vector<string>& extra_args) {
    std::vector<WebDashType::RunReturn> ret;

    // Store extra args in runconfig for $.args() substitution
    runconfig.extra_args = extra_args;

    //
    // Actions can be specified in multiple ways:
    //
    //      <task_name>
    //      :<task_name>
    //      webdash.config.json:blabla
    //          Executes a task specified in the current directory's webdash.config.json.
    //
    //      $.thisDir()/relative-from-thisDir()/webdash.config.json:blabla
    //      $.thisDir()/relative-from-thisDir()/:blabla
    //          Specified in the config file of a directory.
    //
    //      ./path-relative-to-webdash-root/x/y/z/webdash.config.json:blabla
    //      ./path-relative-to-webdash-root/x/y/z/:blabla
    //
    runconfig.TaskRetriever = [&](const string webdash_command_arg) -> optional<WebDashConfigTask> {

        // The case where the action starts with ':' - look in current config
         if (webdash_command_arg[0] == ':') {
            return GetTask(webdash_command_arg.substr(1));
        } else {
            // For cross-config task references (like "../:task" or "path/to/config:task"),
            // return nullopt to force execution via shell. This ensures the target config
            // sets up its own TaskRetriever with proper context for sub-task resolution.
            // The shell execution will invoke `webdash <path>:<task>` which properly
            // initializes the target config's context including $.thisDir() and task lookup.
            return nullopt;
        }

        return nullopt;
    };

    for (WebDashConfigTask& task : _tasks) {
        if (!task.IsValid()) {
            continue;
        }

        // When run_only_with_frequency is set, run ALL tasks that have a frequency
        // (the task's ShouldExecuteTimewise will handle the actual filtering).
        // Otherwise, only run the specific command or the "all" task.
        if (runconfig.run_only_with_frequency) {
            // Let the task decide if it should run based on frequency/timing
            ret.push_back(task.Run(runconfig));
        } else if (task.GetName() == command_name) {
            ret.push_back(task.Run(runconfig));
        } else if (command_name == "" && task.GetName() == WebDashConst::Public::kDefaultTaskName) {
            ret.push_back(task.Run(runconfig));
        }
    }

    return ret;
}


inline std::optional<WebDashConfig> GetBestMatchingConfig(const std::filesystem::path& path,
                                                          const bool check_ancestry)
{
    // If the path doesn't exist, try to auto-clone from modules
    if (!std::filesystem::exists(path)) {
        TryAutoCloneModule(path);
    }
    // If the path exists but is a directory without webdash.config.json, also try to clone
    else if (std::filesystem::is_directory(path)) {
        auto config_path = path / WebDashConst::Public::kProjectConfigFilename;
        if (!std::filesystem::exists(config_path)) {
            TryAutoCloneModule(path);
        }
    }

    /**
     * If `path` is NOT a directory, do NOT do any smart config-searching.
     */

    if (std::filesystem::exists(path) && !filesystem::is_directory(path)) {
        auto tconfig = WebDashConfig(path);

        if (!tconfig.LastLodingSucceeded())
        {
            WebDash().Log(WebDashType::LogType::ERR, "Invalid config: " + path.string());
            return nullopt;
        }

        return tconfig;
    }

    // If still doesn't exist after auto-clone attempt, bail
    if (!std::filesystem::exists(path)) {
        return nullopt;
    }

    /**
     * Smart config-searching. Will go through the directory's ancestry chain if `check_ancestry` is `true`.
     */

    filesystem::path current_directory = filesystem::canonical(path);

    for (size_t go_up_index = 0;
        go_up_index <= /*random MAX value*/ 30;
        go_up_index++) {

        std::filesystem::path fs_config_path = current_directory / WebDashConst::Public::kProjectConfigFilename;

        auto tconfig = WebDashConfig(fs_config_path);
        if (tconfig.LastLodingSucceeded()) return tconfig;

        if (current_directory == current_directory.root_directory()) break;
        current_directory = current_directory.parent_path();

        if (!check_ancestry) break;
    }

    return nullopt;
}


std::optional<ConfigAndCommand> GetConfigAndCommand(const vector<string>& arguments) {

    // Extract extra arguments (everything after the first argument)
    vector<string> extra_args;
    if (arguments.size() > 1) {
        extra_args = vector<string>(arguments.begin() + 1, arguments.end());
    }

    /**
     * Case 0) No arguments given. Use root to attempt to identify a configuration file and use `all` as the command.
     */

    if (arguments.size() == 0) {
        auto config = GetBestMatchingConfig("./");
        if (config) return std::make_tuple(config.value(), WebDashConst::Public::kDefaultTaskName, vector<string>{});
        else return nullopt;
    }

    /**
     * Case 1.0) Execute a command in the **current** directory. Examples:
     *
     *           `webdash <command>`
     *           `webdash :<command>`
     */

    if (arguments[0].find('/') == string::npos)
    {
        auto path = ParseArgumentForPathWithCommandPrecedence(arguments[0]);
        string command_str = ParseArgumentForCommandWithPathPrecedence(arguments[0]).value_or(arguments[0]);

        if (!path) {
            auto config = GetBestMatchingConfig("./", false);
            if (config) return std::make_tuple(config.value(), command_str, extra_args);
        }
    }

    /**
     * Case 1.1) Checks for a config in **remote** directory. Examples:
     *
     *           `webdash <remote dir>` - Command ":all" is returned.
     *           `webdash <remote dir>:<command>`.
     *           `webdash <remote file> - The remote file WebDash JSON file. Command ":all" is returned.
     *           `webdash <remote file>:<command> - Similar to previous example.
     */


    {
        string path_str = ParseArgumentForPathWithCommandPrecedence(arguments[0]).value_or(arguments[0]);
        string command_str = ParseArgumentForCommandWithPathPrecedence(arguments[0]).value_or(WebDashConst::Public::kDefaultTaskName);

        {
            auto config = GetBestMatchingConfig(path_str, false);
            if (config) return std::make_tuple(config.value(), command_str, extra_args);
        }
    }

    /**
     * Case 1.2) Equivalent to case 1.1) but also looks for the `webdash.config.json` file in the ancestry of the
     *           current directory.
     */

    {
        auto path = ParseArgumentForPathWithCommandPrecedence(arguments[0]);
        string command_str = ParseArgumentForCommandWithPathPrecedence(arguments[0]).value_or(arguments[0]);

        if (!path) {
            auto config = GetBestMatchingConfig("./", false);
            if (config) return std::make_tuple(config.value(), command_str, extra_args);
        }
    }

    return nullopt;
}
