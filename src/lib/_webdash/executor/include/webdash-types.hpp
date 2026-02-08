#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <optional>
#include <sys/types.h>  // For uid_t, gid_t
using namespace std;

class WebDashConfigTask;

namespace WebDashType {

    /**
     * @enum Type of write action. @note The "End" action closes the file.
     */
    enum class StorageWriteType {
        Append,
        Clear,
        End
    };

    /**
     * @enum When reading a file, one of the following modifier can be selected
     *       to adjust the reading behavior. For example, if reading fails, in
     *       case of JSON, we return "{}" instead of an empty string "".
     */
    enum class StorageReadType {
        JSON,
        Text
    };

    /**
     * @enum Types of logging messages.
     */
    enum class LogType {
        INFO = 1,
        ERR = 2,
        WARN = 3,
        NOTIFY = 4,
        DEBUG = 5
    };

    /**
     * @enum Map all enum logging types to their respective string identifier.
     */
    inline const std::map<LogType, string> kLogTypeToString {
        { WebDashType::LogType::INFO,   "info"  },
        { WebDashType::LogType::ERR,    "error" },
        { WebDashType::LogType::WARN,   "warn"  },
        { WebDashType::LogType::NOTIFY, "notify"},
        { WebDashType::LogType::DEBUG,  "debug"}
    };

    /**
     * @struct Information to manage a GIT project (module) or setup-based dependency.
     */
    struct GitProjectMetadata {
        string source;       // Git URL (new: "git" field, legacy: "source")
        string destination;  // Local path (new: "path" field, legacy: "destination")
        string branch;       // Git branch (optional)
        string webdash_task; // Task to run after clone (new: "run" field, legacy: "exec")
        vector<string> actions; // Array of shell commands for setup (alternative to git clone)
        string wdir;         // Working directory for actions (supports $.path() for self-reference)
        bool do_register = false;   // Auto-register with WebDash server

        // Check if this is a setup-based dependency (no git, has actions)
        bool IsSetupBased() const { return source.empty() && !actions.empty(); }
    };

    /**
     * @struct @todo
     */
    struct RunReturn {
        int return_code;
        string output;
        string task_name;
        bool skipped;

        RunReturn()
        {
            return_code = 0;
            output = "";
            task_name = "";
            skipped = false;
        }
    };

    /**
     * @struct Represents a dependency, which can be either a simple string path
     *         or an inline module declaration with git URL.
     */
    struct Dependency {
        string path;              // Path to dependency (with :task suffix)
        string git;               // Git URL (optional - for inline module)
        string branch;            // Git branch (optional)

        // Simple string dependency
        Dependency(const string& p) : path(p) {}

        // Full inline module dependency
        Dependency(const string& p, const string& g, const string& b = "")
            : path(p), git(g), branch(b) {}

        bool IsInlineModule() const { return !git.empty(); }
    };

    /**
     * @struct RunConfig - Configuration for how a task should be executed.
     */
    struct RunConfig {
        bool run_only_with_frequency = false;
        bool skip_frequency_check = false;  // If true, execute regardless of frequency/timing
        bool redirect_output_to_str = false;
        bool verbose = false;  // Show detailed execution info (Forking, TASKID, etc.)
        std::function<std::optional<WebDashConfigTask>(string)> TaskRetriever;
        std::vector<std::string> extra_args;  // Command-line arguments for $.args() substitution

        // Optional: run as a specific user (uid/gid). If set, the forked process will
        // switch to this user before executing. Requires root privileges.
        std::optional<uid_t> run_as_uid;
        std::optional<gid_t> run_as_gid;
        std::optional<std::string> run_as_home;  // HOME directory for the user
    };

    using StoreWriteChannel = std::function<void(WebDashType::StorageWriteType, string)>;
}
