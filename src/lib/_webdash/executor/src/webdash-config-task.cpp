#include "webdash-utils.hpp"
#include "webdash-config-task.hpp"
#include "webdash-core.hpp"
#include "webdash-config.hpp"
#include "webdash-constants.hpp"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <functional>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <iostream>
#include <pwd.h>
#include <unordered_map>
#include <tuple>
using namespace std;

namespace {

string SanitizeLogFilenameComponent(const string& value) {
    string sanitized;
    sanitized.reserve(value.size());

    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            sanitized.push_back(static_cast<char>(ch));
        } else {
            sanitized.push_back('_');
        }
    }

    if (sanitized.empty()) {
        sanitized = "task";
    }

    constexpr size_t kMaxComponentLength = 80;
    if (sanitized.size() > kMaxComponentLength) {
        sanitized.resize(kMaxComponentLength);
    }

    return sanitized;
}

filesystem::path GetBackgroundOutputLogPath(const string& taskid) {
    string task_name = taskid;
    const auto separator_pos = taskid.find('#');
    if (separator_pos != string::npos && separator_pos + 1 < taskid.size()) {
        task_name = taskid.substr(separator_pos + 1);
    }

    std::ostringstream hash_stream;
    hash_stream << std::hex << std::hash<string>{}(taskid);

    filesystem::path log_directory = WebDashCore::Get().GetWebDashRootDirectory();
    log_directory /= WebDashConst::Public::kTemporaryStorageDir;
    log_directory /= WebDashConst::Public::kLoggingSubdir;
    log_directory /= _WEBDASH_PROJECT_NAME_;
    filesystem::create_directories(log_directory);

    const string filename = string(WebDashConst::Public::kLogFilenamePrefix)
                          + "background."
                          + SanitizeLogFilenameComponent(task_name)
                          + "."
                          + hash_stream.str()
                          + ".txt";

    return log_directory / filename;
}

/**
 * @brief Reads a string field from a task's JSON, if present.
 *
 * Unlike the previous blanket `try { ... } catch (...) {}` pattern, this
 * distinguishes "field absent" (returns nullopt, no log — perfectly normal
 * for optional fields) from "field present but the wrong type" (logs a clear
 * ERROR naming the task, field, and the type actually found, then returns
 * nullopt) — the latter is almost always a config typo/mistake that was
 * previously being silently ignored.
 *
 * @returns The field's string value, or nullopt if absent or wrong type.
 */
std::optional<string> GetStringField(const json& task_config, const string& taskid, const char* field) {
    auto it = task_config.find(field);
    if (it == task_config.end()) return std::nullopt;

    if (!it->is_string()) {
        WebDash().Log(WebDashType::LogType::ERR,
            "T| " + taskid + ": field [" + field + "] must be a string, but found " +
            string(it->type_name()) + ". Value ignored.");
        return std::nullopt;
    }

    return it->get<std::string>();
}

/**
 * @brief Same as GetStringField(), but for boolean fields (e.g., "silent": true).
 */
std::optional<bool> GetBoolField(const json& task_config, const string& taskid, const char* field) {
    auto it = task_config.find(field);
    if (it == task_config.end()) return std::nullopt;

    if (!it->is_boolean()) {
        WebDash().Log(WebDashType::LogType::ERR,
            "T| " + taskid + ": field [" + field + "] must be a boolean (true/false), but found " +
            string(it->type_name()) + ". Value ignored.");
        return std::nullopt;
    }

    return it->get<bool>();
}

} // namespace


WebDashConfigTask::WebDashConfigTask(WebDashConfig* config,
                                     const string taskid,
                                     json task_config)
{
    WebDash().Log(WebDashType::LogType::DEBUG, "Loading Task: " + taskid);

    this->_config_path = config->GetPath();
    this->_config_dir = config->GetDirectory().string();
    this->_config_env = config->GetConfigEnv();
    this->_taskid = taskid;
    this->_is_valid = true;

    /**
     * Parse the webdash.config.jsonc file.
     */

    try {
        const string name = task_config["name"].get<std::string>();
        this->_name = name;
    }
    catch (...) {
        if (!task_config.contains("name")) {
            WebDash().Log(WebDashType::LogType::ERR, "T| " + taskid + ": field missing [name].");
        } else {
            WebDash().Log(WebDashType::LogType::ERR,
                "T| " + taskid + ": field [name] must be a string, but found " +
                string(task_config["name"].type_name()) + ".");
        }
        _is_valid = false;
        return;
    }

    {
        bool has_action = false;

        if (auto action = GetStringField(task_config, taskid, "action")) {
            this->_actions.push_back(*action);
            has_action = true;
        }

        if (task_config.contains("actions")) {
            const json& actions_json = task_config["actions"];
            if (!actions_json.is_array()) {
                WebDash().Log(WebDashType::LogType::ERR,
                    "T| " + taskid + ": field [actions] must be an array of strings, but found " +
                    string(actions_json.type_name()) + ". Ignored.");
            } else {
                // Presence of a valid (even empty) "actions" array counts as
                // having actions - an empty array is a legitimate pattern for
                // condition-only tasks that do nothing on success.
                has_action = true;
                for (const auto& action_entry : actions_json) {
                    if (!action_entry.is_string()) {
                        WebDash().Log(WebDashType::LogType::ERR,
                            "T| " + taskid + ": an entry in [actions] must be a string, but found " +
                            string(action_entry.type_name()) + ". Ignored.");
                        continue;
                    }
                    this->_actions.push_back(action_entry.get<std::string>());
                }
            }
        }

        if (!has_action) {
            _is_valid = false;
            WebDash().Log(WebDashType::LogType::ERR, "T| " + taskid + ": field missing [actions] (need 'action' or 'actions').");
        }
    }

    // Parse actions-else (runs when condition is false)
    if (auto action = GetStringField(task_config, taskid, "action-else")) {
        this->_actions_else.push_back(*action);
    }

    if (task_config.contains("actions-else")) {
        const json& actions_json = task_config["actions-else"];
        if (!actions_json.is_array()) {
            WebDash().Log(WebDashType::LogType::ERR,
                "T| " + taskid + ": field [actions-else] must be an array of strings, but found " +
                string(actions_json.type_name()) + ". Ignored.");
        } else {
            for (const auto& action_entry : actions_json) {
                if (!action_entry.is_string()) {
                    WebDash().Log(WebDashType::LogType::ERR,
                        "T| " + taskid + ": an entry in [actions-else] must be a string, but found " +
                        string(action_entry.type_name()) + ". Ignored.");
                    continue;
                }
                this->_actions_else.push_back(action_entry.get<std::string>());
            }
        }
    }

    if (task_config.contains("dependencies")) {
        const json& dependencies = task_config["dependencies"];

        if (!dependencies.is_array()) {
            WebDash().Log(WebDashType::LogType::ERR,
                "T| " + taskid + ": field [dependencies] must be an array, but found " +
                string(dependencies.type_name()) + ". Ignored.");
        } else {
            try {
                for (const auto& dependency : dependencies) {
                    if (dependency.is_string()) {
                        // Simple string dependency: "path/to/project:task"
                        string dep_str = dependency.get<std::string>();

                        // Strip legacy ? suffix (now always resolves by default)
                        if (!dep_str.empty() && dep_str.back() == '?') {
                            dep_str = dep_str.substr(0, dep_str.size() - 1);
                        }
                        this->_dependencies.emplace_back(dep_str);
                    } else if (dependency.is_object()) {
                        auto read_dep_field = [&](const char* key) -> string {
                            if (!dependency.contains(key)) return "";
                            if (!dependency[key].is_string()) {
                                WebDash().Log(WebDashType::LogType::ERR,
                                    "T| " + taskid + ": dependency field [" + key + "] must be a string, but found " +
                                    string(dependency[key].type_name()) + ". Ignored.");
                                return "";
                            }
                            return dependency[key].get<std::string>();
                        };

                        string path = read_dep_field(WebDashConst::Public::kModuleFieldPath);
                        string git = read_dep_field(WebDashConst::Public::kModuleFieldGit);
                        string branch = read_dep_field(WebDashConst::Public::kModuleFieldBranch);
                        string run_task = read_dep_field(WebDashConst::Public::kModuleFieldRun);

                        // Construct the full path:task string
                        string full_path = path;
                        if (!run_task.empty()) {
                            full_path += ":" + run_task;
                        }

                        if (!git.empty()) {
                            // Inline module with git
                            this->_dependencies.emplace_back(full_path, git, branch);
                        } else {
                            // Simple path dependency
                            this->_dependencies.emplace_back(full_path);
                        }
                    } else {
                        WebDash().Log(WebDashType::LogType::ERR,
                            "T| " + taskid + ": an entry in [dependencies] must be a string or object, but found " +
                            string(dependency.type_name()) + ". Ignored.");
                    }
                }
            }
            catch (const std::exception& e) {
                WebDash().Log(WebDashType::LogType::ERR, "T| " + taskid + ": failed to parse [dependencies]: " + string(e.what()));
            }
        }
    }

    if (auto frequency = GetStringField(task_config, taskid, "frequency")) {
        this->_frequency = *frequency;
    } else if (!task_config.contains("frequency")) {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": field missing [frequency].");
    }

    if (auto when = GetStringField(task_config, taskid, "when")) {
        this->_when_to_execute = *when;
    } else if (!task_config.contains("when")) {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": field missing [when] (remove this?).");
    }

    if (auto wdir = GetStringField(task_config, taskid, "wdir")) {
        this->_wdir = *wdir;
    } else if (!task_config.contains("wdir")) {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": no working directory (wdir) given.");
    }

    if (auto continue_on_error = GetBoolField(task_config, taskid, "continue_on_error")) {
        this->_continue_on_error = *continue_on_error;
    }

    if (auto val = GetBoolField(task_config, taskid, "notify-dashboard")) {
        this->_notify_dashboard = *val;
    } else if (!task_config.contains("notify-dashboard")) {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": dashboard notification not specified.");
    }

    if (auto val = GetBoolField(task_config, taskid, "allow-execution-as-ancestor")) {
        this->_allow_execution_as_ancestor = *val;
    } else if (!task_config.contains("allow-execution-as-ancestor")) {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": allow-execution-as-ancestor not specified.");
    }

    if (auto val = GetBoolField(task_config, taskid, "background")) {
        this->_background = *val;
    }

    if (auto condition = GetStringField(task_config, taskid, "condition")) {
        this->_condition = *condition;
    }
    // No condition specified - command runs unconditionally

    if (auto user = GetStringField(task_config, taskid, "user")) {
        this->_user = *user;
    }
    // No user specified - run as current user

    if (auto silent = GetBoolField(task_config, taskid, "silent")) {
        this->_silent = *silent;
    }
    // No silent specified - default to verbose output

    if (auto test_case = GetStringField(task_config, taskid, "test-case")) {
        this->_test_case = *test_case;
    }
    // No test-case specified

    if (auto style = GetStringField(task_config, taskid, "executionStyle")) {
        if (*style == "stopOnFirstSuccess") {
            this->_execution_style = ExecutionStyle::StopOnFirstSuccess;
        } else if (*style == "neverStop") {
            this->_execution_style = ExecutionStyle::NeverStop;
        } else if (*style == "stopOnFirstFailure") {
            this->_execution_style = ExecutionStyle::StopOnFirstFailure;
        } else {
            WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": unknown executionStyle '" + *style + "', using default.");
        }
    }
    // No executionStyle specified - use default (StopOnFirstFailure)

    // Parse exit-actions: array of objects mapping exit codes to action lists
    // Format: [{"0": ["action1", "action2"]}, {"*": ["on-failure-action"], "return": 0}]
    try {
        if (task_config.find("exit-actions") != task_config.end()) {
            json exit_actions_json = task_config["exit-actions"];
            if (exit_actions_json.is_array()) {
                for (const auto& exit_action_obj : exit_actions_json) {
                    if (exit_action_obj.is_object()) {
                        // Check for optional "return" field
                        std::optional<int> return_override;
                        if (exit_action_obj.contains("return")) {
                            return_override = exit_action_obj["return"].get<int>();
                        }

                        for (auto it = exit_action_obj.begin(); it != exit_action_obj.end(); ++it) {
                            string code = it.key();
                            // Skip the "return" key - it's not an exit code
                            if (code == "return") continue;

                            const auto& actions = it.value();
                            if (actions.is_array()) {
                                ExitActionEntry entry;
                                for (const auto& action : actions) {
                                    if (action.is_string()) {
                                        entry.actions.push_back(action.get<std::string>());
                                    }
                                }
                                entry.return_override = return_override;
                                this->_exit_actions[code] = entry;
                                WebDash().Log(WebDashType::LogType::DEBUG,
                                    "T| " + taskid + ": loaded exit-action for code '" + code +
                                    "' with " + std::to_string(entry.actions.size()) + " actions" +
                                    (return_override.has_value() ? ", return=" + std::to_string(*return_override) : ""));
                            }
                        }
                    }
                }
            }
        }
    }
    catch (const std::exception& e) {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": failed to parse exit-actions: " + string(e.what()));
    }
    catch (...) {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": failed to parse exit-actions.");
    }

    //
    // Apply all keyword substitutions.
    //

    auto defs = config->GetProfileConfigSubtitutions();
    const auto& profile_entries = WebDashCore::Get().GetKeyValuesFromRootProfile();

    _name = WebDashUtils::ApplySubstitutions(_name, defs);

    for (auto& action : _actions) {
        action = WebDashUtils::ApplySubstitutions(action, defs);
        action = WebDashUtils::ResolvePortReferences(action, profile_entries);
        action = WebDashUtils::ResolveEnvReferences(action);
    }

    for (auto& action : _actions_else) {
        action = WebDashUtils::ApplySubstitutions(action, defs);
        action = WebDashUtils::ResolvePortReferences(action, profile_entries);
        action = WebDashUtils::ResolveEnvReferences(action);
    }

    for (auto& dependency : _dependencies) {
        dependency.path = WebDashUtils::ApplySubstitutions(dependency.path, defs);
        dependency.path = WebDashUtils::ResolvePortReferences(dependency.path, profile_entries);
        dependency.path = WebDashUtils::ResolveEnvReferences(dependency.path);
        // git and branch don't need substitution (they're external URLs)
    }

    if (_wdir.has_value()) {
        _wdir = WebDashUtils::ApplySubstitutions(_wdir.value(), defs);
        _wdir = WebDashUtils::ResolvePortReferences(_wdir.value(), profile_entries);
        _wdir = WebDashUtils::ResolveEnvReferences(_wdir.value());
    }

    // Apply substitutions to exit-actions
    for (auto& [code, entry] : _exit_actions) {
        for (auto& action : entry.actions) {
            action = WebDashUtils::ApplySubstitutions(action, defs);
            action = WebDashUtils::ResolvePortReferences(action, profile_entries);
            action = WebDashUtils::ResolveEnvReferences(action);
        }
    }

    // Apply substitutions to test-case
    if (_test_case.has_value()) {
        _test_case = WebDashUtils::ApplySubstitutions(_test_case.value(), defs);
        _test_case = WebDashUtils::ResolvePortReferences(_test_case.value(), profile_entries);
        _test_case = WebDashUtils::ResolveEnvReferences(_test_case.value());
    }

    // Note: _condition is evaluated at runtime, not substituted here
}

bool is_number(const std::string& s) {
    char* end = 0;
    const double val = strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && val != HUGE_VAL;
}

bool WebDashConfigTask::ShouldExecuteTimewise(WebDashType::RunConfig config) {

    // Manual trigger — always run.
    if (config.skip_frequency_check)
        return true;

    const auto diff = std::chrono::high_resolution_clock::now() - _last_exec_time;
    const auto diff_h = duration_cast<hours>(diff).count();
    const auto diff_ms = duration_cast<milliseconds>(diff).count();

    // We expect frequency because of <run_only_with_frequency> but didn't get any.
    if (config.run_only_with_frequency && !_frequency.has_value())
        return false;

    bool enough_time_passed = true;
    if (_frequency.has_value()) {
        string freqv = _frequency.value();

        if (freqv != "daily" && !is_number(freqv)) {
            WebDash().Log(WebDashType::LogType::INFO, "Malformed frequency field. Skipped.");
            enough_time_passed = false;
        }

        if (diff_h < 24 && freqv == "daily") {
            enough_time_passed = false;
        } else if (diff_ms && is_number(freqv) && (diff_ms < stod(freqv))) {
            enough_time_passed = false;
        }
    }

    if (enough_time_passed == false) {
        return false;
    }

    if (_when_to_execute == "new-day") {
        std::time_t _time_last  = std::chrono::system_clock::to_time_t(_last_exec_time);
        std::time_t _time_today = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        const auto tm_last  = gmtime(&_time_last);
        const auto tm_today = gmtime(&_time_today);

        const int day_last  = tm_last->tm_mday;
        const int day_today = tm_today->tm_mday;

        if (day_last != day_today) {
            return true;
        }
    }


    return true;
}

// wsl.exe -- source ~/.profile && webdash install
WebDashType::RunReturn WebDashConfigTask::Run(WebDashType::RunConfig config, std::string action) {

    WebDashType::RunReturn retval;
    _times_called++;

    // Apply $.args() substitution at runtime
    if (!config.extra_args.empty()) {
        auto shell_escape_single_quotes = [](const string& value) -> string {
            string out;
            out.reserve(value.size() + 2);
            out += "'";
            for (char c : value) {
                if (c == '\'') {
                    out += "'\"'\"'";
                } else {
                    out += c;
                }
            }
            out += "'";
            return out;
        };

        string args_str;
        for (size_t i = 0; i < config.extra_args.size(); ++i) {
            if (i > 0) args_str += " ";
            args_str += shell_escape_single_quotes(config.extra_args[i]);
        }
        action = WebDashUtils::ApplySubstitutions(action, {{WebDashConst::Public::kSubstArgs, args_str}});
    } else {
        // Replace $.args() with empty string if no args provided
        action = WebDashUtils::ApplySubstitutions(action, {{WebDashConst::Public::kSubstArgs, ""}});
    }

    WebDash().Log(WebDashType::LogType::DEBUG, "Executing: " + this->_taskid);
    WebDash().Log(WebDashType::LogType::DEBUG, "    => " + action);

    std::optional<filesystem::path> background_output_log;
    if (_background) {
        try {
            auto log_path = GetBackgroundOutputLogPath(this->_taskid);
            int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log_fd >= 0) {
                close(log_fd);
                background_output_log = log_path;
                WebDash().Log(WebDashType::LogType::INFO,
                    "Background task output for " + this->_taskid + " will be appended to: " + log_path.string());
            } else {
                WebDash().Log(WebDashType::LogType::WARN,
                    "Failed to open background task output log for " + this->_taskid + ": " + strerror(errno));
            }
        }
        catch (const std::exception& e) {
            WebDash().Log(WebDashType::LogType::WARN,
                "Failed to prepare background task output log for " + this->_taskid + ": " + string(e.what()));
        }
    }

    if (config.verbose) {
        cout << "Forking... " << endl;
    }

    int filedes[2];
    // We create a pipe to be shared with two processes.
    if (pipe(filedes) == -1)
    {
        perror("pipe");
        exit(1);
    }

    // Close pipe automatically after child done calling exec.
    if (fcntl(filedes[0], F_SETFD, FD_CLOEXEC) == -1) {
        perror("fcntl");
        exit(1);
    }

    const pid_t pid = fork();
    if (pid == 0) {

        /**
         * Reset SIGCHLD to default behavior. The parent process (e.g., webdash-server)
         * may have set SIGCHLD to SIG_IGN which would break the double-fork pattern
         * and cause child processes to be reaped incorrectly.
         */
        signal(SIGCHLD, SIG_DFL);

        /**
         * For background processes, do a double-fork to fully detach from parent.
         * This prevents zombie processes and allows the parent to exit immediately.
         */
        if (_background) {
            // Create new session so child is not killed when parent terminal closes
            setsid();

            // Second fork - grandchild becomes the actual background process
            pid_t pid2 = fork();
            if (pid2 > 0) {
                // First child exits immediately
                _exit(0);
            } else if (pid2 < 0) {
                perror("second fork");
                _exit(1);
            }
            // Grandchild continues below to exec the command

            // Detach stdin, but keep stdout/stderr in a task-specific WebDash log file.
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
            }

            int output_fd = -1;
            if (background_output_log.has_value()) {
                output_fd = open(background_output_log.value().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            }

            if (output_fd >= 0) {
                dup2(output_fd, STDOUT_FILENO);
                dup2(output_fd, STDERR_FILENO);
                if (output_fd > STDERR_FILENO) close(output_fd);
            } else if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
            }

            if (devnull >= 0) {
                if (devnull > STDERR_FILENO) close(devnull);
            }
        }

        /**
         * Switch to a different user if specified in the run config.
         * This must be done before chdir to ensure proper permissions.
         */
        if (config.run_as_uid.has_value()) {
            // Set HOME environment variable if provided
            if (config.run_as_home.has_value()) {
                setenv("HOME", config.run_as_home.value().c_str(), 1);
            }

            // Set group ID first (must be done before setuid)
            if (config.run_as_gid.has_value()) {
                if (setgid(config.run_as_gid.value()) != 0) {
                    perror("WebDashConfigTask::Run!setgid");
                    WebDash().Log(WebDashType::LogType::ERR, "Failed to setgid to: " + to_string(config.run_as_gid.value()));
                    exit(1);
                }
            }

            // Set user ID
            if (setuid(config.run_as_uid.value()) != 0) {
                perror("WebDashConfigTask::Run!setuid");
                WebDash().Log(WebDashType::LogType::ERR, "Failed to setuid to: " + to_string(config.run_as_uid.value()));
                exit(1);
            }

            WebDash().Log(WebDashType::LogType::DEBUG, "Switched to uid=" + to_string(config.run_as_uid.value()));
        }

        /**
         * Export this task's config-level "env" entries (see
         * docs/configuration.md#environment-variables) as real environment
         * variables for the process about to be exec'd - the same mechanism
         * as the profile's "env", just scoped to this config instead of
         * baked into the shell init script. Applied here (in the forked
         * child, right before exec) rather than in the parent process, so
         * there's no risk of leaking into unrelated sibling tasks/configs.
         */
        for (const auto& [key, value] : _config_env) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        /**
         * Change the CWD to the task's working directory. If `wdir` wasn't
         * explicitly set, default to the config file's own directory (see
         * docs/configuration.md#working-directory) rather than silently
         * inheriting whatever cwd the calling webdash process happened to be
         * in, which produced results that depended on the invoking shell's
         * location (e.g., a plain "webdash :sometask" action run as part of
         * another task's actions).
         */

        {
            const string effective_wdir = _wdir.has_value() ? _wdir.value() : _config_dir;
            if (chdir(effective_wdir.c_str()) != 0) {
                perror ("WebDashConfigTask::Run!chdir: Specified work directory does not exist?");
                WebDash().Log(WebDashType::LogType::ERR, "Failed to set cwd to: " + effective_wdir);
                exit(1);
            }

            WebDash().Log(WebDashType::LogType::DEBUG, "Working directory set to: " + effective_wdir);
        }

        // Always use bash -c for consistent shell behavior (supports redirects, pipes, globs, etc.)
        std::vector<std::string> execParts = {"bash", "-c", action};

        if (execParts.empty()) {
            cerr << "WebDashConfigTask::Run: Empty command" << endl;
            exit(1);
        }

        const char **paramList = new const char*[execParts.size() + 1];

        for (unsigned int i = 0; i < execParts.size(); ++i)
            paramList[i] = execParts[i].c_str();

        paramList[execParts.size()] = NULL;

        // Show task info (TASKID, CWD, CALL) unless silent mode
        if (!_silent) {
            cout << "\033[1;33m-----------------" << endl;
            cout << "  TASKID: " << _taskid << endl;
            cout << "  CWD:    " << std::filesystem::current_path() << endl;
            cout << "  CALL:   `" << paramList[0];
            for (unsigned int i = 1; i < execParts.size(); ++i) {
                cout << " " << execParts[i];
            }
            cout << "`" << endl;
            cout << "-----------------\033[0m" << endl;
        }

        {
            // If we are to redirect output to a string, create a copy of filedes[1] to STDOUT_FILENO (standard output).
            if (config.redirect_output_to_str) {
                while ((dup2(filedes[1], STDOUT_FILENO) == -1) && (errno == EINTR)) {}
                while ((dup2(filedes[1], STDERR_FILENO) == -1) && (errno == EINTR)) {}
            }

            // Close now children copies. We don't need them. We only used them to share our standard output with filedes[1] (which parent still utilizes).
            close(filedes[1]);
            close(filedes[0]);
        }

        if (execvp(paramList[0], (char**)paramList) < 0) {
            perror ("WebDashConfigTask::Run!execvp");
        }

        exit(1);

    } else if (pid > 0) {
        close(filedes[1]);

        // parent
        if (config.redirect_output_to_str) {

            retval.output = "";

            while (1) {
                char buffer[43];
                int len = read(filedes[0], buffer, 42);
                if ( len < 0 ) {
                    if (errno == EINTR) {
                        continue;
                    }
                    else {
                        perror("read");
                        close(filedes[0]);
                        exit(1);
                    }
                }
                else if ( len == 0 ) {
                    break;
                }
                else {
                    std::string data(buffer, len);
                    retval.output += data;
                }
            }
        }

        close(filedes[0]);
    }

    /**
     * Wait for child to finish before exiting. Return the object containing the
     * processing result (e.g., status code of executed processes).
     *
     * For background processes, we still wait for the first child (which exits
     * immediately after double-forking), but we don't wait for the grandchild.
     * This prevents zombie processes.
     *
     * Note: If SIGCHLD is set to SIG_IGN (e.g., in webdash-server), children are
     * auto-reaped and waitpid() may return -1 with ECHILD. This is not an error.
     */

    int status = 0;
    const pid_t wpid = waitpid(pid, &status, 0);

    // Handle case where child was auto-reaped (SIGCHLD=SIG_IGN)
    if (wpid == -1 && errno == ECHILD) {
        // Child was already reaped, assume success for background tasks
        // For non-background tasks, we can't know the exit status
        retval.return_code = _background ? 0 : 0;  // Assume success
        return retval;
    }

    if (_background) {
        // Background process - first child already exited, grandchild is running detached
        retval.return_code = wpid == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (retval.return_code == 0 && background_output_log.has_value() && !_silent) {
            cout << "Background task started. Output: " << background_output_log.value() << endl;
        }
        return retval;
    }

    retval.return_code = wpid == pid &&
                            WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return retval;
}


WebDashType::RunReturn WebDashConfigTask::Run(WebDashType::RunConfig config) {

    WebDashType::RunReturn ret;

    ret.task_name = this->_taskid;

    // Check timing FIRST before any expensive operations like user lookups
    if (!ShouldExecuteTimewise(config)) {
        if (_print_skip_has_happened == false)
        {
            WebDash().Log(WebDashType::LogType::DEBUG, "Skipping: " + this->_taskid);
            // Only show elapsed time and omit message if task has a frequency (i.e., is a scheduled task)
            if (_frequency.has_value()) {
                const auto diff = std::chrono::high_resolution_clock::now() - _last_exec_time;
                const auto diff_ms = duration_cast<milliseconds>(diff).count();
                const double diff_sec = diff_ms / 1000.0;
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(1) << diff_sec;
                WebDash().Log(WebDashType::LogType::DEBUG, "Was executed " + oss.str() + " seconds ago.");
                WebDash().Log(WebDashType::LogType::DEBUG, "....omitting further similar reports until next execution passed.");
            }
            _print_skip_has_happened = true;
        }

        ret.skipped = true;
        return ret;
    }

    // If a user is specified in the task config, look up uid/gid/home and apply to run config
    // Task-level user takes precedence over registration-level user
    // Use static cache to avoid repeated NSS lookups for the same username
    if (_user.has_value()) {
        static std::unordered_map<std::string, std::tuple<uid_t, gid_t, std::string>> user_cache;

        auto it = user_cache.find(_user.value());
        if (it != user_cache.end()) {
            config.run_as_uid = std::get<0>(it->second);
            config.run_as_gid = std::get<1>(it->second);
            config.run_as_home = std::get<2>(it->second);
            WebDash().Log(WebDashType::LogType::DEBUG, "Task will run as user: " + _user.value() + " (uid=" + to_string(config.run_as_uid.value()) + ", cached)");
        } else {
            struct passwd* pwd = getpwnam(_user.value().c_str());
            if (pwd != nullptr) {
                config.run_as_uid = pwd->pw_uid;
                config.run_as_gid = pwd->pw_gid;
                config.run_as_home = pwd->pw_dir;
                user_cache[_user.value()] = {pwd->pw_uid, pwd->pw_gid, pwd->pw_dir};
                WebDash().Log(WebDashType::LogType::DEBUG, "Task will run as user: " + _user.value() + " (uid=" + to_string(pwd->pw_uid) + ")");
            } else {
                WebDash().Log(WebDashType::LogType::ERR, "User not found: " + _user.value());
                ret.return_code = 1;
                ReportTestCaseResult(ret);
                return ret;
            }
        }
    }

    // Check condition before executing
    // If actions-else is defined but no condition, that's an error
    if (!_actions_else.empty() && !_condition.has_value()) {
        cerr << "ERROR: 'actions-else' defined without 'condition' in task: " << _taskid << endl;
        ret.return_code = 1;
        return ret;  // Do not go through exit-actions
    }

    if (!EvaluateCondition()) {
        // Condition is false - run actions-else if defined
        if (!_actions_else.empty()) {
            if (!_silent) {
                cout << "\033[1;33m-----------------" << endl;
                cout << "  TASKID: " << _taskid << endl;
                cout << "  CONDITION FALSE: " << _condition.value_or("(none)") << " - running actions-else" << endl;
                cout << "-----------------\033[0m" << endl;
            }

            // Run actions-else (without exit-actions processing)
            for (const string& action : _actions_else) {
                auto ret_sub = Run(config, action);
                ret.output += ret_sub.output;
                ret.return_code |= ret_sub.return_code;

                if (ret.return_code && !_continue_on_error) {
                    return ret;  // Do not go through exit-actions
                }
            }
            return ret;  // Do not go through exit-actions
        }

        // No actions-else defined, just skip
        if (!_silent) {
            cout << "\033[1;33m-----------------" << endl;
            cout << "  TASKID: " << _taskid << endl;
            cout << "  SKIPPED: Condition not satisfied: " << _condition.value_or("(none)") << endl;
            cout << "-----------------\033[0m" << endl;
        }
        return ret;
    }

    _print_skip_has_happened = false;
    _last_exec_time = std::chrono::high_resolution_clock::now();

    if (_notify_dashboard) {
        IWebDash::Notify(_taskid);
    }

    // If this task has a wdir that points to a different directory,
    // update the TaskRetriever to look up tasks in that directory's config.
    // This allows wdir to act as a context switch for task references.
    WebDashType::RunConfig effective_config = config;
    if (_wdir.has_value()) {
        std::filesystem::path wdir_path(_wdir.value());
        auto wdir_config = GetBestMatchingConfig(wdir_path, false);
        if (wdir_config.has_value()) {
            // Create a new TaskRetriever that looks in the wdir's config for :task references
            effective_config.TaskRetriever = [wdir_config_copy = wdir_config.value(), original_retriever = config.TaskRetriever](const string& webdash_command_arg) mutable -> std::optional<WebDashConfigTask> {
                if (!webdash_command_arg.empty() && webdash_command_arg[0] == ':') {
                    // Look up in the wdir's config
                    return wdir_config_copy.GetTask(webdash_command_arg.substr(1));
                } else {
                    // For path-based references, use the original retriever
                    return original_retriever(webdash_command_arg);
                }
            };
        }
    }

    for (int i = 0; i < (int)_dependencies.size(); ++i) {
        const auto& dep = _dependencies[i];

        // Extract path without :task suffix for existence check
        string check_path = dep.path;
        auto colon_pos = check_path.find(':');
        if (colon_pos != string::npos) {
            check_path = check_path.substr(0, colon_pos);
        }

        // If path doesn't exist, try to auto-resolve from profile
        if (!check_path.empty() && !std::filesystem::exists(check_path)) {
            bool resolved = false;
            auto profile_deps = WebDashCore::Get().GetExternalGitProjects();
            for (const auto& profile_dep : profile_deps) {
                if (profile_dep.destination == check_path) {
                    if (profile_dep.IsSetupBased()) {
                        // Setup-based dependency (actions)
                        std::cout << "Auto-resolving dependency: " << check_path << std::endl;

                        // Create working directory
                        string working_dir = profile_dep.wdir.empty() ? profile_dep.destination : profile_dep.wdir;
                        std::filesystem::create_directories(working_dir);

                        // Run setup actions
                        for (const auto& action : profile_dep.actions) {
                            std::cout << "  Running: " << action << std::endl;
                            string full_cmd = "cd \"" + working_dir + "\" && " + action;
                            auto result = WebDashUtils::ExecuteCommand(full_cmd);
                            if (!result.output.empty()) {
                                std::cout << "  Output: " << result.output << std::endl;
                            }
                            if (result.Failed()) {
                                std::cerr << "ERROR: Action failed: " << action << std::endl;
                                ret.return_code = 1;
                                if (!_continue_on_error) {
                                    ReportTestCaseResult(ret);
                                    return ret;
                                }
                            }
                        }
                        resolved = true;
                    } else if (!profile_dep.source.empty()) {
                        // Git-based dependency
                        std::cout << "Auto-cloning dependency: " << check_path << std::endl;

                        string clone_cmd = "git clone";
                        if (!profile_dep.branch.empty()) {
                            clone_cmd += " -b " + profile_dep.branch;
                        }
                        clone_cmd += " \"" + profile_dep.source + "\" \"" + profile_dep.destination + "\"";

                        std::cout << "  Running: " << clone_cmd << std::endl;
                        auto result = WebDashUtils::ExecuteCommand(clone_cmd);
                        if (result.Failed()) {
                            std::cerr << "ERROR: Clone failed: " << clone_cmd << std::endl;
                            if (!result.output.empty()) {
                                std::cerr << "  Output: " << result.output << std::endl;
                            }
                            ret.return_code = 1;
                            if (!_continue_on_error) {
                                ReportTestCaseResult(ret);
                                return ret;
                            }
                        } else if (!profile_dep.actions.empty()) {
                            // Run post-clone actions if any
                            string working_dir = profile_dep.wdir.empty() ? profile_dep.destination : profile_dep.wdir;
                            std::cout << "  Running post-clone actions in: " << working_dir << std::endl;

                            for (const auto& action : profile_dep.actions) {
                                std::cout << "    Running: " << action << std::endl;
                                string full_cmd = "cd \"" + working_dir + "\" && " + action;
                                auto action_result = WebDashUtils::ExecuteCommand(full_cmd);
                                if (!action_result.output.empty()) {
                                    std::cout << "    Output: " << action_result.output << std::endl;
                                }
                                if (action_result.Failed()) {
                                    std::cerr << "ERROR: Post-clone action failed: " << action << std::endl;
                                    ret.return_code = 1;
                                    if (!_continue_on_error) {
                                        ReportTestCaseResult(ret);
                                        return ret;
                                    }
                                }
                            }
                        }
                        resolved = true;
                    }
                    break;
                }
            }
        }

        // If this is a path-only dependency (no :task suffix), we're done with it
        if (check_path == dep.path) {
            // Check that the path exists
            if (!std::filesystem::exists(check_path)) {
                WebDash().Log(WebDashType::LogType::ERR,
                    "Required path does not exist: " + check_path + " (required by " + _taskid + ")");
                std::cerr << "ERROR: Required path does not exist: " << check_path << std::endl;
                ret.return_code = 1;
                if (!_continue_on_error) {
                    ReportTestCaseResult(ret);
                    return ret;
                }
            }
            continue;  // Path-only dependency done, move to next
        }

        // For inline modules, try to clone first if needed
        if (dep.IsInlineModule()) {
            // Extract path without :task suffix for clone check
            string path_only = dep.path;
            colon_pos = path_only.find(':');
            if (colon_pos != string::npos) {
                path_only = path_only.substr(0, colon_pos);
            }

            if (!std::filesystem::exists(path_only)) {
                WebDash().Log(WebDashType::LogType::INFO,
                    "Auto-cloning inline module: " + dep.git + " -> " + path_only);

                string clone_cmd = "git clone";
                if (!dep.branch.empty()) {
                    clone_cmd += " -b " + dep.branch;
                }
                clone_cmd += " \"" + dep.git + "\" \"" + path_only + "\"";

                auto result = WebDashUtils::ExecuteCommand(clone_cmd);
                if (result.Failed()) {
                    WebDash().Log(WebDashType::LogType::ERR,
                        "Failed to clone inline module: " + dep.git + " -> " + path_only);
                    if (!result.output.empty()) {
                        WebDash().Log(WebDashType::LogType::ERR, "Clone output: " + result.output);
                    }
                    ret.return_code = 1;
                    if (!_continue_on_error) {
                        ReportTestCaseResult(ret);
                        return ret;
                    }
                    continue;
                }
            }
        }

        auto task = effective_config.TaskRetriever(dep.path);

        if (task.has_value()) {
            auto ret_sub = task.value().Run(effective_config);
            ret.output += ret_sub.output;
            ret.return_code |= ret_sub.return_code;

            if (ret.return_code && !_continue_on_error) {
                return ret;
            }
        } else {
            // TaskRetriever returned nullopt - this could be a cross-config reference
            // Check if it looks like a cross-config task reference (path:task pattern)
            bool is_cross_config = false;
            if (dep.path.find(':') != string::npos) {
                // Has a task separator - likely a cross-config reference like "path/to/config:task"
                is_cross_config = true;
            }

            if (is_cross_config) {
                // Execute via webdash command
                string effective_action = "webdash " + dep.path;
                auto ret_sub = Run(effective_config, effective_action);
                ret.output += ret_sub.output;
                ret.return_code |= ret_sub.return_code;

                if (ret.return_code && !_continue_on_error) {
                    return ret;
                }
            } else {
                // Dependency not found - this is a fatal error
                WebDash().Log(WebDashType::LogType::ERR,
                    "Dependency not found: " + dep.path + " (required by " + _taskid + ")");
                ret.return_code = 1;
                if (!_continue_on_error) {
                    ReportTestCaseResult(ret);
                    return ret;
                }
            }
        }
    }

    for (int i = 0; i < (int)_actions.size(); ++i) {

        const string action = _actions[i];
        auto maybesubtask = effective_config.TaskRetriever(action);

        int action_return_code = 0;

        if (maybesubtask.has_value()) {
            auto ret_sub = maybesubtask.value().Run(effective_config);
            ret.output += ret_sub.output;
            action_return_code = ret_sub.return_code;
        }
        else {
            // Check if this looks like a cross-config webdash task reference
            // Valid patterns: "../:task", "./subdir:task", "path/config.json:task", "/abs/path:task"
            string effective_action = action;
            bool is_cross_config_task = false;

            // Cross-config refs must start with a path indicator:
            // - "./" or "../" (relative path)
            // - "/" (absolute path)
            // - Or a path ending in ".json:" (explicit config file)
            if (action.size() > 2) {
                bool starts_with_path = (action.substr(0, 2) == "./" ||
                                         action.substr(0, 3) == "../" ||
                                         action[0] == '/');
                bool has_task_separator = action.find(':') != string::npos;

                if (starts_with_path && has_task_separator) {
                    is_cross_config_task = true;
                }
            }

            if (is_cross_config_task) {
                effective_action = "webdash " + action;
            }

            auto ret_sub = Run(effective_config, effective_action);
            ret.output += ret_sub.output;
            action_return_code = ret_sub.return_code;
        }

        // Handle execution style
        switch (_execution_style) {
            case ExecutionStyle::StopOnFirstFailure:
                ret.return_code |= action_return_code;
                if (action_return_code && !_continue_on_error) {
                    std::cerr << "ERROR: Action failed with exit code " << action_return_code << ": " << action << std::endl;
                    auto final_ret = RunExitActions(ret.return_code, effective_config);
                    final_ret.task_name = ret.task_name;
                    ReportTestCaseResult(final_ret);
                    return final_ret;
                }
                break;

            case ExecutionStyle::StopOnFirstSuccess:
                if (action_return_code == 0) {
                    // Success! We're done.
                    ret.return_code = 0;
                    auto final_ret = RunExitActions(ret.return_code, effective_config);
                    final_ret.task_name = ret.task_name;
                    ReportTestCaseResult(final_ret);
                    return final_ret;
                }
                // Keep track of the last failure code
                ret.return_code = action_return_code;
                break;

            case ExecutionStyle::NeverStop:
                ret.return_code |= action_return_code;
                // Continue regardless of success/failure
                break;
        }
    }

    // For StopOnFirstSuccess, if we get here, all actions failed
    if (_execution_style == ExecutionStyle::StopOnFirstSuccess && ret.return_code != 0) {
        std::cerr << "ERROR: All actions failed" << std::endl;
    }

    // Run exit-actions based on final return code
    auto final_ret = RunExitActions(ret.return_code, effective_config);
    final_ret.task_name = ret.task_name;

    // Report test-case result if specified
    ReportTestCaseResult(final_ret);

    return final_ret;
}


WebDashType::RunReturn WebDashConfigTask::RunExitActions(int exit_code, WebDashType::RunConfig config) {
    WebDashType::RunReturn ret;
    ret.return_code = exit_code;
    ret.task_name = this->_taskid;

    // If no exit-actions defined, just return the original result
    if (_exit_actions.empty()) {
        return ret;
    }

    // Look for exact match first
    string exit_code_str = std::to_string(exit_code);
    ExitActionEntry* entry_to_run = nullptr;

    if (_exit_actions.count(exit_code_str)) {
        entry_to_run = &_exit_actions[exit_code_str];
    }
    // If no exact match and exit code is non-zero, try wildcard "*"
    else if (exit_code != 0 && _exit_actions.count("*")) {
        entry_to_run = &_exit_actions["*"];
    }

    if (entry_to_run == nullptr) {
        return ret;
    }

    if (config.verbose) {
        cout << "\033[1;36m--- Running exit-actions for exit code " << exit_code_str << " ---\033[0m" << endl;
    }

    for (const string& action : entry_to_run->actions) {
        auto maybesubtask = config.TaskRetriever(action);

        if (maybesubtask.has_value()) {
            auto ret_sub = maybesubtask.value().Run(config);
            ret.output += ret_sub.output;
            // Don't modify the original return code from exit-actions
        }
        else {
            // Execute as shell command
            string effective_action = action;
            bool is_cross_config_task = false;

            if (action.size() > 2) {
                bool starts_with_path = (action.substr(0, 2) == "./" ||
                                         action.substr(0, 3) == "../" ||
                                         action[0] == '/');
                bool has_task_separator = action.find(':') != string::npos;

                if (starts_with_path && has_task_separator) {
                    is_cross_config_task = true;
                }
            }

            if (is_cross_config_task) {
                effective_action = "webdash " + action;
            }

            auto ret_sub = Run(config, effective_action);
            ret.output += ret_sub.output;
            // Don't modify the original return code from exit-actions
        }
    }

    // Apply return override if specified
    if (entry_to_run->return_override.has_value()) {
        ret.return_code = entry_to_run->return_override.value();
        if (config.verbose) {
            cout << "\033[1;36m--- Exit code overridden to " << ret.return_code << " ---\033[0m" << endl;
        }
    }

    return ret;
}


void WebDashConfigTask::ReportTestCaseResult(const WebDashType::RunReturn& ret) {
    if (_test_case.has_value()) {
        if (ret.return_code == 0) {
            cout << "\033[1;32m[PASS]\033[0m " << _test_case.value() << endl;
        } else {
            cout << "\033[1;31m[FAIL]\033[0m " << _test_case.value() << endl;
        }
    }
}


bool WebDashConfigTask::EvaluateCondition() {
    if (!_condition.has_value()) {
        return true;  // No condition = always run
    }

    string cond = _condition.value();

    // Check for NOT operator (!)
    bool negate = false;
    if (!cond.empty() && cond[0] == '!') {
        negate = true;
        cond = cond.substr(1);
    }

    // Parse $.isPortUnused(<port_or_identifier>)
    const string prefix = "$.isPortUnused(";
    if (cond.find(prefix) == 0 && cond.back() == ')') {
        string port_str = cond.substr(prefix.length(), cond.length() - prefix.length() - 1);

        // Remove any whitespace
        port_str.erase(remove_if(port_str.begin(), port_str.end(), ::isspace), port_str.end());

        int port = -1;

        // Check if it's a profile port reference: $.port(identifier)
        const string port_prefix = "$.port(";
        if (port_str.find(port_prefix) == 0 && port_str.back() == ')') {
            string port_id = port_str.substr(port_prefix.length(), port_str.length() - port_prefix.length() - 1);

            // Look up in profile under "ports.<identifier>"
            const auto& entries = WebDashCore::Get().GetKeyValuesFromRootProfile();
            bool found = false;
            for (const auto& entry : entries) {
                const auto& tokens = entry.GetTokens();
                if (tokens.size() == 2 && tokens[0] == "ports" && tokens[1] == port_id) {
                    try {
                        port = std::stoi(entry.GetValue());
                        found = true;
                        break;
                    } catch (...) {
                        WebDash().Log(WebDashType::LogType::ERR, "Invalid port value in profile for: " + port_id);
                        return false;
                    }
                }
            }
            // The static file server has a sensible built-in default (see
            // WebDashConst::Public::kDefaultStaticFileServerPort) so profiles
            // aren't required to define it.
            if (!found && port_id == WebDashConst::Public::kProfilePortStaticFileServer) {
                port = WebDashConst::Public::kDefaultStaticFileServerPort;
                found = true;
            }
            if (!found) {
                WebDash().Log(WebDashType::LogType::ERR, "Port identifier not found in profile: " + port_id);
                return false;
            }
        } else {
            // Try to parse as literal number
            try {
                port = std::stoi(port_str);
            } catch (...) {
                WebDash().Log(WebDashType::LogType::ERR, "Failed to parse port number in condition: " + port_str);
                return false;
            }
        }

        if (port < 1 || port > 65535) {
            WebDash().Log(WebDashType::LogType::ERR, "Invalid port number in condition: " + to_string(port));
            return false;
        }
        bool result = WebDashUtils::IsPortUnused(port);
        return negate ? !result : result;
    }

    WebDash().Log(WebDashType::LogType::ERR, "Unknown condition: " + cond);
    return false;
}
