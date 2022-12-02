#pragma once

#include <chrono>

#include <nlohmann/json.hpp>

#include "webdash-config-task.hpp"
#include "webdash-types.hpp"

class WebDashConfig;

using json = nlohmann::json;
using namespace std::chrono;


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
        vector<string> _dependencies;
        string _name;
        std::optional<string> _wdir;

        // Per default, ::time_point is initialized to epoch.
        std::chrono::high_resolution_clock::time_point _last_exec_time;

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
};