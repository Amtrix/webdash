#pragma once

#include <chrono>
#include <map>

#include <nlohmann/json.hpp>

#include "webdash-config-task.hpp"
#include "webdash-types.hpp"

class WebDashConfig;

using json = nlohmann::json;
using namespace std::chrono;


/**
 * @enum Execution style for multiple actions.
 */
enum class ExecutionStyle {
    StopOnFirstFailure,  // Default: stop when any action fails
    StopOnFirstSuccess,  // Stop when any action succeeds (useful for fallback chains)
    NeverStop            // Run all actions regardless of success/failure
};


/**
 * @class Representative of a single webdash task from within a config file.
 */
class WebDashConfigTask {
    public:

        /**
         * @returns A WebDash config task.
         */
        WebDashConfigTask(WebDashConfig*, const string, json);


        /**
         * @brief If the task has a scheduled execution AND enough time has
         * passed for next execution, returns `true`. Otherwise, returns
         * `false`.
         *
         * @param config The task's execution parameters.
         * @returns true if a there are no time properties blocking the
         *          execution of the task, false otherwise.
         */
        bool ShouldExecuteTimewise(WebDashType::RunConfig config);

        WebDashType::RunReturn Run(WebDashType::RunConfig config, std::string action);

        WebDashType::RunReturn Run(WebDashType::RunConfig config = {});

        string GetName() { return _name; }

        bool IsValid() { return _is_valid; }

        bool CanRunAsAncestor() { return _allow_execution_as_ancestor; }

    private:

        string _taskid;
        std::optional<string> _frequency;
        vector<string> _actions;
        vector<string> _actions_else;  // Actions to run when condition is false
        vector<WebDashType::Dependency> _dependencies;
        string _name;
        std::optional<string> _wdir;

        // Initialize to current time so first execution happens immediately.
        std::chrono::high_resolution_clock::time_point _last_exec_time =
            std::chrono::high_resolution_clock::now() - std::chrono::hours(24 * 365);

        // Exactly that. Counts the number of times ::Run() was called.
        int _times_called = 0;

        // We want to print once if a task execution was skipped. We use this flag to
        // skip such further logging.
        bool _print_skip_has_happened = false;

        // The task might be invalid due to some parameters wrongly set in the JSON.
        // Not restricted to this example only.
        bool _is_valid = true;

        bool _notify_dashboard = false;

        string _when_to_execute;

        string _config_path;

        bool _continue_on_error = false;

        bool _allow_execution_as_ancestor = false;

        bool _background = false;  // If true, run command in background (don't wait for completion)

        std::optional<string> _condition;  // Condition that must be true to run command

        std::optional<string> _user;  // User to run command as (username)

        bool _silent = false;  // If true, suppress verbose task output (TASKID, CWD, CALL)

        std::optional<string> _test_case;  // If set, report pass/fail with this test case name

        ExecutionStyle _execution_style = ExecutionStyle::StopOnFirstFailure;

        // Exit actions: map exit codes (or "*" for any non-zero) to actions and optional return override
        // Key is exit code as string ("0", "1", etc.) or "*" for wildcard
        struct ExitActionEntry {
            vector<string> actions;
            std::optional<int> return_override;  // If set, overrides the final exit code
        };
        std::map<string, ExitActionEntry> _exit_actions;

        /**
         * @brief Evaluates the condition string (e.g., $.isPortUnused(8080)).
         * @returns true if condition is satisfied or no condition set, false otherwise.
         */
        bool EvaluateCondition();

        /**
         * @brief Runs exit-actions based on the provided return code.
         * @param exit_code The exit code from the main actions.
         * @param config The run configuration.
         * @returns Updated RunReturn with exit-action results.
         */
        WebDashType::RunReturn RunExitActions(int exit_code, WebDashType::RunConfig config);

        /**
         * @brief Reports the test-case result if a test-case is specified.
         * @param ret The return value to report and pass through.
         */
        void ReportTestCaseResult(const WebDashType::RunReturn& ret);
};
