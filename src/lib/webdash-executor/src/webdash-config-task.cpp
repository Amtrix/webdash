#include "webdash-utils.hpp"
#include "webdash-config-task.hpp"
#include "webdash-core.hpp"
#include "webdash-config.hpp"
#include "webdash-constants.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <sstream>
#include <ctime>
#include <iostream>
using namespace std;


WebDashConfigTask::WebDashConfigTask(WebDashConfig* config,
                                     const string taskid,
                                     json task_config)
{
    WebDash().Log(WebDashType::LogType::DEBUG, "Loading Task: " + taskid);

    this->_config_path = config->GetPath();
    this->_taskid = taskid;
    this->_is_valid = true;

    /**
     * Parse the webdash.config.json file.
     */

    try {
        const string name = task_config["name"].get<std::string>();
        this->_name = name;
    }
    catch (...) {
        WebDash().Log(WebDashType::LogType::ERR, "T| " + taskid + ": field missing [name].");
        _is_valid = false;
        return;
    }

    {
        bool has_action = false;
        try {
            const string action = task_config["action"].get<std::string>();
            this->_actions.push_back(action);
            has_action = true;
        }
        catch (...){}

        try {
            json actions = task_config["actions"];

            for (auto action : actions)
                this->_actions.push_back(action.get<std::string>());
            has_action = true;
        }
        catch (...){}

        if (!has_action) {
            _is_valid = false;
            WebDash().Log(WebDashType::LogType::ERR, "T| " + taskid + ": field missing [actions].");
        }
    }

    // Parse actions-else (runs when condition is false)
    try {
        if (task_config.find("action-else") != task_config.end()) {
            const string action = task_config["action-else"].get<std::string>();
            this->_actions_else.push_back(action);
        }
    }
    catch (...){}

    try {
        if (task_config.find("actions-else") != task_config.end()) {
            json actions = task_config["actions-else"];
            for (auto action : actions)
                this->_actions_else.push_back(action.get<std::string>());
        }
    }
    catch (...){}

    try {
        json dependencies = task_config["dependencies"];

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
                string path = "";
                string git = "";
                string branch = "";
                string run_task = "";

                if (dependency.contains(WebDashConst::Public::kModuleFieldPath)) {
                    path = dependency[WebDashConst::Public::kModuleFieldPath].get<std::string>();
                }
                if (dependency.contains(WebDashConst::Public::kModuleFieldGit)) {
                    git = dependency[WebDashConst::Public::kModuleFieldGit].get<std::string>();
                }
                if (dependency.contains(WebDashConst::Public::kModuleFieldBranch)) {
                    branch = dependency[WebDashConst::Public::kModuleFieldBranch].get<std::string>();
                }
                if (dependency.contains(WebDashConst::Public::kModuleFieldRun)) {
                    run_task = dependency[WebDashConst::Public::kModuleFieldRun].get<std::string>();
                }

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
            }
        }
    }
    catch (...)
    {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": field missing [dependencies].");
    }


    try {
        const string frequency = task_config["frequency"].get<std::string>();
        this->_frequency = frequency;
    }
    catch (...)
    {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": field missing [frequency].");
    }

    try {
        const string when = task_config["when"].get<std::string>();
        this->_when_to_execute = when;
    }
    catch (...) {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": field missing [when] (remove this?).");
    }

    try {
        const string wdir = task_config["wdir"].get<std::string>();
        this->_wdir = wdir;
    }
    catch (...)
    {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": no working directory (wdir) given.");
    }

    try {
        const bool continue_on_error = task_config["continue_on_error"].get<bool>();
        this->_continue_on_error = continue_on_error;
    }
    catch (...){}

    try {
        const bool val = task_config["notify-dashboard"].get<bool>();
        this->_notify_dashboard = val;
    }
    catch (...)
    {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": dashboard notification not specified.");
    }

    try {
        const bool val = task_config["allow-execution-as-ancestor"].get<bool>();
        this->_allow_execution_as_ancestor = val;
    }
    catch (...) {
        WebDash().Log(WebDashType::LogType::WARN, "T| " + taskid + ": dashboard notification not specified.");
    }

    try {
        const string condition = task_config["condition"].get<std::string>();
        this->_condition = condition;
    }
    catch (...) {
        // No condition specified - command runs unconditionally
    }

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
    _name = WebDashUtils::ApplySubstitutions(_name, defs);

    for (auto& action : _actions) {
        action = WebDashUtils::ApplySubstitutions(action, defs);
    }

    for (auto& action : _actions_else) {
        action = WebDashUtils::ApplySubstitutions(action, defs);
    }

    for (auto& dependency : _dependencies) {
        dependency.path = WebDashUtils::ApplySubstitutions(dependency.path, defs);
        // git and branch don't need substitution (they're external URLs)
    }

    if (_wdir.has_value()) {
        _wdir = WebDashUtils::ApplySubstitutions(_wdir.value(), defs);
    }

    // Apply substitutions to exit-actions
    for (auto& [code, entry] : _exit_actions) {
        for (auto& action : entry.actions) {
            action = WebDashUtils::ApplySubstitutions(action, defs);
        }
    }

    // Note: _condition is evaluated at runtime, not substituted here
}

bool is_number(const std::string& s) {
    char* end = 0;
    const double val = strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && val != HUGE_VAL;
}

bool WebDashConfigTask::ShouldExecuteTimewise(WebDashType::RunConfig config) {

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
        string args_str;
        for (size_t i = 0; i < config.extra_args.size(); ++i) {
            if (i > 0) args_str += " ";
            // Quote arguments containing spaces
            if (config.extra_args[i].find(' ') != string::npos) {
                args_str += "\"" + config.extra_args[i] + "\"";
            } else {
                args_str += config.extra_args[i];
            }
        }
        action = WebDashUtils::ApplySubstitutions(action, {{WebDashConst::Public::kSubstArgs, args_str}});
    } else {
        // Replace $.args() with empty string if no args provided
        action = WebDashUtils::ApplySubstitutions(action, {{WebDashConst::Public::kSubstArgs, ""}});
    }

    WebDash().Log(WebDashType::LogType::DEBUG, "Executing: " + this->_taskid);
    WebDash().Log(WebDashType::LogType::DEBUG, "    => " + action);

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
         * Change the CWD if required by the task.
         */

        if (_wdir.has_value()) {
            if (chdir(_wdir.value().c_str()) != 0) {
                perror ("WebDashConfigTask::Run!chdir: Specified work directory does not exist?");
                WebDash().Log(WebDashType::LogType::ERR, "Failed to set cwd to: " + _wdir.value());
                exit(1);
            }

            WebDash().Log(WebDashType::LogType::DEBUG, "Working directory set to: " + _wdir.value());
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

        // Always show task info (TASKID, CWD, CALL)
        cout << "\033[1;33m-----------------" << endl;
        cout << "  TASKID: " << _taskid << endl;
        cout << "  CWD:    " << std::filesystem::current_path() << endl;
        cout << "  CALL:   `" << paramList[0];
        for (unsigned int i = 1; i < execParts.size(); ++i) {
            cout << " " << execParts[i];
        }
        cout << "`" << endl;
        cout << "-----------------\033[0m" << endl;

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
     */

    int status;
    const pid_t wpid = waitpid(pid, &status, 0);

    retval.return_code = wpid == pid &&
                            WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return retval;
}


WebDashType::RunReturn WebDashConfigTask::Run(WebDashType::RunConfig config) {

    WebDashType::RunReturn ret;

    if (!ShouldExecuteTimewise(config)) {
        if (_print_skip_has_happened == false)
        {
            WebDash().Log(WebDashType::LogType::DEBUG, "Skipping: " + this->_taskid);
            WebDash().Log(WebDashType::LogType::DEBUG, "Was executed XYZ milliseconds ago.");
            WebDash().Log(WebDashType::LogType::DEBUG, "....ommitting further similar reports until next execution passed.");
            _print_skip_has_happened = true;
        }

        return ret;
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
            cout << "\033[1;33m-----------------" << endl;
            cout << "  TASKID: " << _taskid << endl;
            cout << "  CONDITION FALSE: " << _condition.value_or("(none)") << " - running actions-else" << endl;
            cout << "-----------------\033[0m" << endl;

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
        cout << "\033[1;33m-----------------" << endl;
        cout << "  TASKID: " << _taskid << endl;
        cout << "  SKIPPED: Condition not satisfied: " << _condition.value_or("(none)") << endl;
        cout << "-----------------\033[0m" << endl;
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
                                if (!_continue_on_error) return ret;
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
                            if (!_continue_on_error) return ret;
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
                                    if (!_continue_on_error) return ret;
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
                if (!_continue_on_error) return ret;
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
                    if (!_continue_on_error) return ret;
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
                if (!_continue_on_error) return ret;
            }
        }
    }

    for (int i = 0; i < (int)_actions.size(); ++i) {

        const string action = _actions[i];
        auto maybesubtask = effective_config.TaskRetriever(action);

        if (maybesubtask.has_value()) {
            auto ret_sub = maybesubtask.value().Run(effective_config);
            ret.output += ret_sub.output;
            ret.return_code |= ret_sub.return_code;
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
            ret.return_code |= ret_sub.return_code;
        }

        if (ret.return_code && !_continue_on_error) {
            std::cerr << "ERROR: Action failed with exit code " << ret.return_code << ": " << action << std::endl;
            // Run exit-actions even on early failure
            return RunExitActions(ret.return_code, effective_config);
        }
    }

    // Run exit-actions based on final return code
    return RunExitActions(ret.return_code, effective_config);
}


WebDashType::RunReturn WebDashConfigTask::RunExitActions(int exit_code, WebDashType::RunConfig config) {
    WebDashType::RunReturn ret;
    ret.return_code = exit_code;

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

    // Parse $.isPortUnused(<port>)
    const string prefix = "$.isPortUnused(";
    if (cond.find(prefix) == 0 && cond.back() == ')') {
        string port_str = cond.substr(prefix.length(), cond.length() - prefix.length() - 1);

        // Remove any whitespace
        port_str.erase(remove_if(port_str.begin(), port_str.end(), ::isspace), port_str.end());

        try {
            int port = std::stoi(port_str);
            if (port < 1 || port > 65535) {
                WebDash().Log(WebDashType::LogType::ERR, "Invalid port number in condition: " + port_str);
                return false;
            }
            bool result = WebDashUtils::IsPortUnused(port);
            return negate ? !result : result;
        } catch (...) {
            WebDash().Log(WebDashType::LogType::ERR, "Failed to parse port number in condition: " + port_str);
            return false;
        }
    }

    WebDash().Log(WebDashType::LogType::ERR, "Unknown condition: " + cond);
    return false;
}
