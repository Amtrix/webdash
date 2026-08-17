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
#include <regex>
#include <nlohmann/json.hpp>
using namespace std;


namespace {

    std::optional<std::string> DeriveStorageScopeFromConfigPath(const std::filesystem::path& config_filepath) {
        const std::string normalized = config_filepath.lexically_normal().generic_string();
        static const std::regex pattern(R"((?:^|/)src/(bin|static|lib)/([^/]+)(?:/|$))");
        std::smatch match;
        if (std::regex_search(normalized, match, pattern) && match.size() >= 3) {
            const std::string bucket = match[1].str();
            const std::string first_child = match[2].str();
            if (!bucket.empty() && !first_child.empty()) {
                return bucket + "/" + first_child;
            }
        }

        return std::nullopt;
    }

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
     *        Handles paths like "/foo/bar" matching "/foo/bar/webdash.config.jsonc"
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
     *        Searches the "modules" array in webdash.profile.jsonc for a matching path.
     * @param path The path that doesn't exist.
     * @returns true if successfully cloned, false otherwise.
     */
    bool TryAutoCloneModule(const std::filesystem::path& path) {
        // Check if the path already has valid content:
        // - If it's a file, it exists
        // - If it's a directory, it must contain webdash.config.jsonc
        if (std::filesystem::exists(path)) {
            if (std::filesystem::is_directory(path)) {
                // Directory exists - check if it has a project config (.jsonc or legacy .json)
                if (WebDashCore::ResolveProjectConfigPath(path).has_value()) {
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
                // Check if the module destination already has a valid project config
                // (.jsonc or legacy .json). If so, don't try to clone - the module is
                // already set up, we just need to access a subdirectory that may not exist yet
                if (WebDashCore::ResolveProjectConfigPath(module.destination).has_value()) {
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
    std::filesystem::path resolved_filepath = config_filepath;

    // Backward-compat: if the requested .jsonc doesn't exist, fall back to a
    // legacy .json sibling (same stem) - e.g. external/older projects not yet
    // renamed still work.
    if (!std::filesystem::exists(resolved_filepath) && resolved_filepath.extension() == ".jsonc") {
        std::filesystem::path json_fallback = resolved_filepath;
        json_fallback.replace_extension(".json");
        if (std::filesystem::exists(json_fallback)) {
            resolved_filepath = json_fallback;
        }
    }

    // Check if the file exists before trying to canonicalize
    if (!std::filesystem::exists(resolved_filepath)) {
        _loading_failed = true;
        _config_filepath = resolved_filepath;
        return;
    }

    auto canonical_config_filepath = std::filesystem::canonical(resolved_filepath);

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

    _project_identifier = config_filepath.parent_path().filename().string();
    if (auto derived_scope = DeriveStorageScopeFromConfigPath(config_filepath); derived_scope.has_value()) {
        _project_identifier = derived_scope.value();
    }

    ifstream configStream;
    try {
        configStream.open(config_filepath.c_str(), ifstream::in);
    }
    catch (...) {
        throw WebDashException::ConfigJsonParseError("Failed to open the config file.");
    }

    json json_config;
    try {
        // Accept JSONC: "//" and "/* */" comments plus trailing commas, in
        // addition to plain JSON. Supported natively by nlohmann/json (no
        // extra dependency) via the ignore_comments/ignore_trailing_commas
        // parse flags.
        json_config = json::parse(configStream, nullptr, true, true, true);
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

    // Scope for $.webdash.* is path-derived from src/(bin|static|lib)/<first-child>.

    // Parse top-level "vars": named constants referenced as $.var(name) from
    // this config's own actions/wdir/dependencies. Values may use
    // $.rootDir(), $.thisDir(), $.webdash.*(), $.port(), and $.env() -
    // resolved once here (vars can't reference other vars, to avoid ordering
    // issues/cycles).
    //
    // $.var(name) also checks the profile's own top-level "vars" section
    // (same mechanism the profile already offers for "ports" - a value
    // defined once, usable from every project) - so a var doesn't have to be
    // copy-pasted into every config that needs it. If the same name is
    // defined in both, this config's own value wins.
    _local_vars.clear();
    {
        auto base_substitutions = GetProfileConfigSubtitutions();
        const auto& profile_entries = WebDashCore::Get().GetKeyValuesFromRootProfile();
        std::set<string> config_defined_names;

        if (json_config.contains("vars")) {
            if (!json_config["vars"].is_object()) {
                WebDash().Log(WebDashType::LogType::ERR,
                    "Config '" + config_filepath.string() + "': top-level [vars] must be an object, but found " +
                    string(json_config["vars"].type_name()) + ". Ignored.");
            } else {
                for (auto it = json_config["vars"].begin(); it != json_config["vars"].end(); ++it) {
                    if (!it.value().is_string()) {
                        WebDash().Log(WebDashType::LogType::ERR,
                            "Config '" + config_filepath.string() + "': vars[" + it.key() + "] must be a string, but found " +
                            string(it.value().type_name()) + ". Ignored.");
                        continue;
                    }

                    string resolved = it.value().get<std::string>();
                    resolved = WebDashUtils::ApplySubstitutions(resolved, base_substitutions);
                    resolved = WebDashUtils::ResolvePortReferences(resolved, profile_entries);
                    resolved = WebDashUtils::ResolveEnvReferences(resolved);

                    _local_vars.push_back({"$.var(" + it.key() + ")", resolved});
                    config_defined_names.insert(it.key());
                }
            }
        }

        // Profile-level vars: any "vars.<name>" entry in webdash.profile.jsonc,
        // skipping names this config already defines itself.
        for (const auto& entry : profile_entries) {
            const auto& tokens = entry.GetTokens();
            if (tokens.size() != 2 || tokens[0] != "vars") continue;
            if (config_defined_names.count(tokens[1])) continue;

            string resolved = WebDashUtils::ApplySubstitutions(entry.GetValue(), base_substitutions);
            resolved = WebDashUtils::ResolvePortReferences(resolved, profile_entries);
            resolved = WebDashUtils::ResolveEnvReferences(resolved);

            _local_vars.push_back({"$.var(" + tokens[1] + ")", resolved});
        }
    }

    // Parse top-level "env": real environment variables exported (via
    // setenv()) for any process that runs one of this config's tasks - the
    // same mechanism as the profile's "env", just scoped to this config
    // instead of baked into the shell init script.
    _config_env.clear();
    if (json_config.contains("env")) {
        if (!json_config["env"].is_object()) {
            WebDash().Log(WebDashType::LogType::ERR,
                "Config '" + config_filepath.string() + "': top-level [env] must be an object, but found " +
                string(json_config["env"].type_name()) + ". Ignored.");
        } else {
            auto base_substitutions = GetProfileConfigSubtitutions();
            const auto& profile_entries = WebDashCore::Get().GetKeyValuesFromRootProfile();

            for (auto it = json_config["env"].begin(); it != json_config["env"].end(); ++it) {
                if (!it.value().is_string()) {
                    WebDash().Log(WebDashType::LogType::ERR,
                        "Config '" + config_filepath.string() + "': env[" + it.key() + "] must be a string, but found " +
                        string(it.value().type_name()) + ". Ignored.");
                    continue;
                }

                string resolved = it.value().get<std::string>();
                resolved = WebDashUtils::ApplySubstitutions(resolved, base_substitutions);
                resolved = WebDashUtils::ResolvePortReferences(resolved, profile_entries);
                resolved = WebDashUtils::ResolveEnvReferences(resolved);

                _config_env.push_back({it.key(), resolved});
            }
        }
    }

    // Parse network-shared flag (top-level config property)
    // Accepts either a boolean (true → use directory name) or a string (custom alias).
    try {
        if (json_config.contains(WebDashConst::Public::kConfigFieldNetworkShared)) {
            auto& val = json_config[WebDashConst::Public::kConfigFieldNetworkShared];
            if (val.is_boolean() && val.get<bool>()) {
                // true → use the directory name as the project alias
                _network_shared = config_filepath.parent_path().filename().string();
            } else if (val.is_string()) {
                // string → use as explicit alias
                _network_shared = val.get<string>();
            }
        }
    } catch (...) {
        // Not critical, default to empty (not shared)
    }

    // Parse network-shared-sourcedir (optional directory override)
    try {
        if (json_config.contains(WebDashConst::Public::kConfigFieldNetworkSharedSourceDir) &&
            json_config[WebDashConst::Public::kConfigFieldNetworkSharedSourceDir].is_string()) {
            _network_shared_sourcedir = json_config[WebDashConst::Public::kConfigFieldNetworkSharedSourceDir].get<string>();

            // Resolve WebDash variables ($.rootDir(), $.thisDir(), etc.)
            auto substitutions = GetProfileConfigSubtitutions();
            _network_shared_sourcedir = WebDashUtils::ApplySubstitutions(_network_shared_sourcedir, substitutions);
        }
    } catch (...) {
        // Not critical, default to empty (use config directory)
    }

    // Parse top-level dependencies (applies to all commands in this config)
    json top_level_deps;
    try {
        top_level_deps = json_config["dependencies"];
    } catch (...) {
        // No top-level dependencies, that's fine
    }

    // Parse top-level defaults (field values applied to every command that
    // doesn't already define that same field itself). Keeps repetitive fields
    // like "user" or "wdir" from having to be copy-pasted onto every command.
    json top_level_defaults;
    try {
        top_level_defaults = json_config["defaults"];
    } catch (...) {
        // No top-level defaults, that's fine
    }

    int command_index = 0;

    for (auto json_command : json_commands) {
        try {
            // Apply config-level defaults for any field the command doesn't
            // already set itself. "name" and "dependencies" are excluded:
            // names must be unique per-command, and dependencies are merged
            // additively above/below rather than defaulted.
            if (!top_level_defaults.is_null() && top_level_defaults.is_object()) {
                for (auto it = top_level_defaults.begin(); it != top_level_defaults.end(); ++it) {
                    const string& key = it.key();
                    if (key == "name" || key == "dependencies") continue;
                    if (!json_command.contains(key)) {
                        json_command[key] = it.value();
                    }
                }
            }

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

    const auto root = WebDashCore::Get().GetWebDashRootDirectory();
    std::filesystem::path temp_static = root / WebDashConst::Public::kTemporaryStorageDir / _project_identifier;
    std::filesystem::path persistent_static = root / WebDashConst::Public::kPersistentStorageDir / _project_identifier;

    config_substitutions.push_back(make_pair(WebDashConst::Public::kSubstWebDashTemporary, temp_static.string()));
    config_substitutions.push_back(make_pair(WebDashConst::Public::kSubstWebDashPersistent, persistent_static.string()));

    config_substitutions.insert(config_substitutions.end(), _local_vars.begin(), _local_vars.end());

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
    //      webdash.config.jsonc:blabla
    //          Executes a task specified in the current directory's webdash.config.jsonc.
    //
    //      $.thisDir()/relative-from-thisDir()/webdash.config.jsonc:blabla
    //      $.thisDir()/relative-from-thisDir()/:blabla
    //          Specified in the config file of a directory.
    //
    //      ./path-relative-to-webdash-root/x/y/z/webdash.config.jsonc:blabla
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
    // If the path exists but is a directory without a project config, also try to clone
    else if (std::filesystem::is_directory(path)) {
        if (!WebDashCore::ResolveProjectConfigPath(path).has_value()) {
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

        std::filesystem::path fs_config_path = WebDashCore::ResolveProjectConfigPath(current_directory)
            .value_or(current_directory / WebDashConst::Public::kProjectConfigFilename);

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
     * Case 1.2) Equivalent to case 1.1) but also looks for the `webdash.config.jsonc` file in the ancestry of the
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
