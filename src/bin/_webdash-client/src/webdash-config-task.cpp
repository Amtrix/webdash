#include "webdash-utils.hpp"
#include "webdash-config-task.hpp"
#include "webdash-core.hpp"
#include "webdash-config.hpp"

#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <sstream>
#include <ctime>
#include <iostream>
using namespace std;


WebDashConfigTask::WebDashConfigTask(WebDashConfig* config,
                                     string taskid,
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

    try {
        json dependencies = task_config["dependencies"];

        for (auto dependency : dependencies)
            this->_dependencies.push_back(dependency.get<std::string>());
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

    //
    // Apply all keyword substitutions.
    //

    auto defs = config->GetProfileConfigSubtitutions();
    _name = WebDashUtils::ApplySubstitutions(_name, defs);

    for (auto& action : _actions) {
        action = WebDashUtils::ApplySubstitutions(action, defs);
    }

    for (auto& dependency : _dependencies) {
        dependency = WebDashUtils::ApplySubstitutions(dependency, defs);
    }

    if (_wdir.has_value()) {
        _wdir = WebDashUtils::ApplySubstitutions(_wdir.value(), defs);
    }
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

    WebDash().Log(WebDashType::LogType::DEBUG, "Executing: " + this->_taskid);
    WebDash().Log(WebDashType::LogType::DEBUG, "    => " + action);

    cout << "Forking... " << endl;

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
        if (_wdir.has_value()) {
            if (chdir(_wdir.value().c_str()) != 0) {
                perror ("WebDashConfigTask::Run!chdir");
                WebDash().Log(WebDashType::LogType::ERR, "Failed to set cwd to: " + _wdir.value());
                exit(1);
            }
            WebDash().Log(WebDashType::LogType::DEBUG, "Working directory set to: " + _wdir.value());
        }

        std::istringstream iss(action.c_str());
        std::vector<std::string> execParts(std::istream_iterator<std::string>{iss},
                                           std::istream_iterator<std::string>());

        const char **paramList = new const char*[execParts.size() + 1];

        for (unsigned int i = 0; i < execParts.size(); ++i)
            paramList[i] = execParts[i].c_str();

        paramList[execParts.size()] = NULL;

        // child
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
    }
    else if (pid > 0) {
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

    _print_skip_has_happened = false;
    _last_exec_time = std::chrono::high_resolution_clock::now();

    if (_notify_dashboard) {
        IWebDash::Notify(_taskid);
    }

    for (int i = 0; i < (int)_dependencies.size(); ++i) {
        auto task = config.TaskRetriever(_dependencies[i]);

        if (task.has_value()) {
            auto ret_sub = task.value().Run(config);
            ret.output += ret_sub.output;
            ret.return_code |= ret_sub.return_code;

            if (ret.return_code && _continue_on_error) {
                return ret;
            }
        }
    }

    for (int i = 0; i < (int)_actions.size(); ++i) {

        const string action = _actions[i];
        auto maybesubtask = config.TaskRetriever(action);

        if (maybesubtask.has_value()) {
            auto ret_sub = maybesubtask.value().Run(config);
            ret.output += ret_sub.output;
            ret.return_code |= ret_sub.return_code;
        }
        else {
            auto ret_sub = Run(config, action);
            ret.output += ret_sub.output;
            ret.return_code |= ret_sub.return_code;
        }

        if (ret.return_code && !_continue_on_error) {
            return ret;
        }
    }

    return ret;
}
