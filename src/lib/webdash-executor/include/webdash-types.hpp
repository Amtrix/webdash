#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
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
     * @struct Information to manage a GIT project.
     */
    struct GitProjectMetadata {
        string source;
        string destination;
        string webdash_task;
        bool do_register;
    };

    /**
     * @struct @todo
     */
    struct RunReturn {
        int return_code;
        string output;

        RunReturn()
        {
            return_code = 0;
            output = "";
        }
    };

    /**
     * @struct @todo
     */
    struct RunConfig {
        bool run_only_with_frequency = false;
        bool redirect_output_to_str = false;
        std::function<std::optional<WebDashConfigTask>(string)> TaskRetriever;
    };

    using StoreWriteChannel = std::function<void(WebDashType::StorageWriteType, string)>;
}