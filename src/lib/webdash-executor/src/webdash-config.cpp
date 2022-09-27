#include "webdash-utils.hpp"
#include "webdash-config.hpp"
#include "webdash-types.hpp"
#include "webdash-core.hpp"

#include <iostream>
#include <fstream>
#include <optional>
#include <nlohmann/json.hpp>
using namespace std;


namespace {

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
    auto canonical_config_filepath = std::filesystem::canonical(config_filepath);

    const auto& previous_config_filepath = _path;
    _path = canonical_config_filepath;

    auto computed_tasks = LoadAndCheckKnownFailures(canonical_config_filepath);

    if (computed_tasks) {
        _loading_failed = false;
        _tasks = computed_tasks.value();
    } else {
        _loading_failed = true;
        _path = previous_config_filepath;
    }
}


std::optional<vector<WebDashConfigTask>> WebDashConfig::LoadAndCheckKnownFailures(const std::filesystem::path path) {
    try {
        return Load(path);
    }
    catch (const WebDashException::ConfigJsonParseError& e) {
        WebDash().Log(WebDashType::LogType::DEBUG, "Not a valid WebDash config file: " + path.string() + ". Reason: " + e.what());
        return nullopt;
    }

    return nullopt;
}


vector<WebDashConfigTask> WebDashConfig::Load(const std::filesystem::path config_filepath) {
    load_failure_reason = LoadFailureReason::NoError;
    vector<WebDashConfigTask> tasks;

    ifstream configStream;
    try {
        configStream.open(config_filepath.c_str(), ifstream::in);
    }
    catch (...) {
        load_failure_reason = LoadFailureReason::FileNotFound;
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

    int command_index = 0;

    for (auto json_command : json_commands) {
        try {
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
    return _path;
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
    config_substitutions.push_back(make_pair("$.thisDir()", WebDashUtils::GetDirectoryOfFilepath(_path)));

    return config_substitutions;
}


void WebDashConfig::Reload() {
    auto computed_tasks = LoadAndCheckKnownFailures(_path);

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


std::optional<WebDashConfigTask> WebDashConfig::GetTask(const string cmdname) {
    for (auto task : _tasks) {
        if (task.GetName() == cmdname) {
            return task;
        } else if (cmdname == "" && task.GetName() == "all") {
            return task;
        }
    }

    return nullopt;
}


std::vector<WebDashType::RunReturn> WebDashConfig::Run(const string command_name, WebDashType::RunConfig runconfig) {
    std::vector<WebDashType::RunReturn> ret;

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
    //      ./path-relative-to-myworld/x/y/z/webdash.config.json:blabla
    //      ./path-relative-to-myworld/x/y/z/:blabla
    //
    runconfig.TaskRetriever = [&](const string webdash_command_arg) -> optional<WebDashConfigTask> {

        // The case where the
         if (webdash_command_arg[0] == ':') {
            return GetTask(webdash_command_arg.substr(1));
        } else {
            try {
                vector<string> arguments;
                arguments.push_back(webdash_command_arg);
                auto config_and_command = GetConfigAndCommand(arguments);

                if (!config_and_command)
                    return nullopt;

                return config_and_command->first.GetTask(config_and_command->second);
            } catch (...) {
                WebDash().Log(WebDashType::LogType::DEBUG, "Not a WebDash task (" + webdash_command_arg + ")");
                return nullopt;
            }
        }

        return nullopt;
    };

    for (WebDashConfigTask& task : _tasks) {
        if (!task.IsValid()) {
            continue;
        }

        // If a specific command name was given, only run that one. Otherwise, find the "all" command and only run that.
        if (task.GetName() == command_name) {
            ret.push_back(task.Run(runconfig));
        } else if (command_name == "" && task.GetName() == "all") {
            ret.push_back(task.Run(runconfig));
        }
    }

    return ret;
}


inline std::optional<WebDashConfig> GetBestMatchingConfig(const std::filesystem::path& path,
                                                          const bool check_ancestry)
{
    /**
     * If `path` is NOT a directory, do NOT do any smart config-searching.
     */

    if (!filesystem::is_directory(path)) {
        auto tconfig = WebDashConfig(path);

        if (!tconfig.LastLodingSucceeded())
        {
            WebDash().Log(WebDashType::LogType::ERR, "Invalid config: " + path.string());
            return nullopt;
        }

        return tconfig;
    }

    /**
     * Smart config-searching. Will go through the directory's ancestry chain if `check_ancestry` is `true`.
     */

    filesystem::path current_directory = filesystem::canonical(path);

    for (size_t go_up_index = 0;
        go_up_index <= /*random MAX value*/ 30;
        go_up_index++) {

        std::filesystem::path fs_config_path = current_directory.string() + "/webdash.config.json";

        auto tconfig = WebDashConfig(fs_config_path);
        if (tconfig.LastLodingSucceeded()) return tconfig;

        if (current_directory == current_directory.root_directory()) break;
        current_directory = current_directory.parent_path();

        if (!check_ancestry) break;
    }

    return nullopt;
}


std::optional<ConfigAndCommand> GetConfigAndCommand(const vector<string>& arguments) {

    if (arguments.size() >= 3)
        return nullopt;

    /**
     * Case 0) No arguments given. Use root to attempt to identify a configuration file and use `all` as the command.
     */

    if (arguments.size() == 0) {
        auto config = GetBestMatchingConfig("./");
        if (config) return std::pair{ config.value(), "all" };
        else return nullopt;
    }

    /**
     * Case 2) Two arguments are given. First one MUST be the path to a config file (or directory in which
     *         webdash.config.json is) and the second one MUST be a command's name.
     */

    if (arguments.size() == 2) {
        string path_str = ParseArgumentForPathWithCommandPrecedence(arguments[0]).value_or(arguments[0]);
        string command_str = arguments[1];

        auto config = GetBestMatchingConfig(path_str);

        if (config) return std::pair{ config.value(), command_str };
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
            if (config) return std::pair{ config.value(), command_str };
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
        string command_str = ParseArgumentForCommandWithPathPrecedence(arguments[0]).value_or("all");

        {
            auto config = GetBestMatchingConfig(path_str, false);
            if (config) return std::pair{ config.value(), command_str };
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
            if (config) return std::pair{ config.value(), command_str };
        }
    }

    return nullopt;
}
