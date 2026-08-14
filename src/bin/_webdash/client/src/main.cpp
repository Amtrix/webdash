// Self
#include "../common/websocket.h"
#include "../common/utils.hpp"

// WebDash
#include <webdash-config.hpp>
#include <webdash-core.hpp>
#include <webdash-constants.hpp>
#include <webdash-exceptions.hpp>

// Standard
#include <cstdio>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <algorithm>
#include <set>
#include <map>
#include <unistd.h>

// External
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace std;
using json = nlohmann::json;


/* extern */ const string _WEBDASH_PROJECT_NAME_ = "webdash-client";
const string _WEBDASH_INTERNAL_CMD_PREFIX = "_internal_:";
const string _WEBDASH_TERMINAL_INIT_FILE_WARNING = "# Warning: This is an automatically generated file by the app-persistent/bin/webdash client. Auto generated. Don't modify.";

constexpr int kSpaceOutCommands = 8;
constexpr int kSpaceOutGroup = 4;

/**
 * Commands to inject into a bash script to print the state of the WebDash process.
 */
const std::string _WEBDASH_TERMINAL_INIT_FILE_APPENDIX = R"(
# Source bash completion if available
if [[ -f "$WEBDASH/app-persistent/bin/completions/webdash.bash" ]]; then
    source "$WEBDASH/app-persistent/bin/completions/webdash.bash"
fi

# Status check
if pgrep -x "webdash-server" > /dev/null
then
    echo 'WebDash: Server (<running>)'
else
    echo 'WebDash: Server (<not running>)'
fi

)";


/**
 * Mapping of WebDash commands to their user-friendly string format.
 * Convention: multi-character options use -- (double dash), single char use - (single dash)
 */
namespace NATIVE_COMMANDS
{
    string CONFIG_REGISTER = "--config.register";
    string CONFIG_UNREGISTER = "--config.unregister";
    string CONFIG_RELOAD_REGISTERED = "--config.reload-registered";
    string HELP = "--help";
    string LIST_CONFIG = "--list-registered-configs";
    string LIST_DEFINITIONS = "--list-definitions";
    string _INT_CREATE_BUILD_INIT = _WEBDASH_INTERNAL_CMD_PREFIX + "create-build-init";
    string _INT_CREATE_PROJECT_CLONER = _WEBDASH_INTERNAL_CMD_PREFIX + "create-project-cloner";
    string PING_SERVER = "--ping-server";
    string INSTALL_MODULE = "--install-module";
    string TOOLS_PORTS_STATUS = "--tools.ports.status";
    string SERVER_PROCESS_ONCE = "--server-process-once";
    string TOOLS_PORTS_FORWARDED_LIST = "--tools.ports.forwarded.list";
    string TOOLS_PORTS_FORWARDED_ADD = "--tools.ports.forwarded.add";
    string TOOLS_PORTS_FORWARDED_REMOVE = "--tools.ports.forwarded.remove";
    string PROFILE_REAPPLY = "--profile.reapply";
    string BACKUP_CREATE = "--backup.create";
    string BACKUP_RESTORE = "--backup.restore";
    string CONFIG_VALIDATE = "--config.validate";
};


/*
 * List of all commands supported by WebDash.
 */
vector<string> WebDashNativeCommands()
{
    vector<string> commands = { NATIVE_COMMANDS::CONFIG_REGISTER,
             NATIVE_COMMANDS::CONFIG_UNREGISTER,
             NATIVE_COMMANDS::CONFIG_RELOAD_REGISTERED,
             NATIVE_COMMANDS::HELP,
             NATIVE_COMMANDS::LIST_CONFIG,
             NATIVE_COMMANDS::LIST_DEFINITIONS,
             NATIVE_COMMANDS::_INT_CREATE_BUILD_INIT,
             NATIVE_COMMANDS::_INT_CREATE_PROJECT_CLONER,
             NATIVE_COMMANDS::PING_SERVER,
             NATIVE_COMMANDS::INSTALL_MODULE,
             NATIVE_COMMANDS::TOOLS_PORTS_STATUS,
             NATIVE_COMMANDS::SERVER_PROCESS_ONCE,
             NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_LIST,
             NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_ADD,
             NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_REMOVE,
             NATIVE_COMMANDS::PROFILE_REAPPLY,
             NATIVE_COMMANDS::BACKUP_CREATE,
             NATIVE_COMMANDS::BACKUP_RESTORE,
             NATIVE_COMMANDS::CONFIG_VALIDATE };

    // Displayed lexicographically (--help and the no-args/no-match listing both
    // pull from this list) so the command set is easy to scan alphabetically,
    // except --help itself, which is pinned last.
    std::sort(commands.begin(), commands.end());
    auto help_it = std::find(commands.begin(), commands.end(), NATIVE_COMMANDS::HELP);
    if (help_it != commands.end()) {
        std::rotate(help_it, help_it + 1, commands.end());
    }

    return commands;
}


/**
 * @brief Checks if the given command is natively understood by WebDash.
 * @param command The name of the command.
 * @returns True iff the given command is native to WebDash.
 */
bool IsInternalCommand(const string command) {
    if (command.length() < _WEBDASH_INTERNAL_CMD_PREFIX.length())
        return false;

    bool prefix_is_internal_identifier =
        command.substr(0, _WEBDASH_INTERNAL_CMD_PREFIX.length()) == _WEBDASH_INTERNAL_CMD_PREFIX;

    return prefix_is_internal_identifier;
}


/**
 * @brief Command: `webdash list-registered-configs`.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool ListRegistered_Command(const vector<string>& arguments) {
    if (arguments.size() != 1)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::LIST_CONFIG)
        return false;

    WebDashConfigList();

    return true;
}


/**
 * @brief Create a script file for initializing a shell to use WebDash. It adds all the environment variables and
 *        especially to the PATH variable.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool CreateBuildInitializer_InternalCommand(const vector<string>& arguments) {
    if (arguments.size() != 1)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::_INT_CREATE_BUILD_INIT)
        return false;

    auto path_additions = WebDashCore::Get().GetEnvPathAdditions();
    auto env_additions = WebDashCore::Get().GetEnvironmentAdditions();

    WebDashCore::Get().WriteToAppStorage("webdash.terminal.init.sh", [&](WebDashType::StoreWriteChannel writer) {

        writer(WebDashType::StorageWriteType::Clear, "");
        writer(WebDashType::StorageWriteType::Append, _WEBDASH_TERMINAL_INIT_FILE_WARNING + "\n");

        /**
         * Create the new PATH environment variable and add the export statement to the script to update it.
         */

        string out = "PATH=$PATH";
        for (auto e : path_additions)
        {
            out = out + ":" + e;
        }

        writer(WebDashType::StorageWriteType::Append, "export " + out + "\n");

        /**
         * Write out the list of environment variables.
         */

        for (auto &[key, val] : env_additions)
        {
            writer(WebDashType::StorageWriteType::Append, "export " + key + "=" + val + "\n");
        }

        /**
         * Write out a command to print the state of the webdash server process. Close the pipe for writing to the
         * script file.
         */

        writer(WebDashType::StorageWriteType::Append, _WEBDASH_TERMINAL_INIT_FILE_APPENDIX);
        writer(WebDashType::StorageWriteType::End, "");
    });

    return true;
}


/**
 * @brief Command: `webdash create-project-cloner`.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool CreateProjectCloner_InternalCommand(const vector<string>& arguments) {
    if (arguments.size() != 1)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::_INT_CREATE_PROJECT_CLONER)
        return false;

    /**
     * Based on webdash.profile.jsonc, create a file that creates a clean clone of all the listed repositories,
     * listed in the 'modules' key.
     *
     * WARNING: This file deletes everything previously cloned.
     */

    auto entries = WebDashCore::Get().GetExternalGitProjects();

    WebDashCore::Get().WriteToAppStorage("initialize-projects.sh", [&](WebDashType::StoreWriteChannel writer) {
        writer(WebDashType::StorageWriteType::Clear, "");
        writer(WebDashType::StorageWriteType::Append, "#!/bin/bash\n");
        writer(WebDashType::StorageWriteType::Append, "# Auto-generated by webdash - initialize modules from webdash.profile.jsonc\n\n");

        for (auto entry : entries) {
            // Check if this is a setup-based dependency (no git URL, has actions)
            if (entry.IsSetupBased()) {
                // For setup-based dependencies, run the actions if destination doesn't exist
                writer(WebDashType::StorageWriteType::Append,
                    "# Setup-based dependency: " + entry.destination + "\n");
                writer(WebDashType::StorageWriteType::Append,
                    "if [ ! -d \"" + entry.destination + "\" ]; then\n");
                writer(WebDashType::StorageWriteType::Append,
                    "    echo \"Running setup for: " + entry.destination + "\"\n");

                // Use wdir if specified, otherwise default to destination
                string working_dir = entry.wdir.empty() ? entry.destination : entry.wdir;
                writer(WebDashType::StorageWriteType::Append,
                    "    mkdir -p \"" + working_dir + "\"\n");
                writer(WebDashType::StorageWriteType::Append,
                    "    cd \"" + working_dir + "\"\n");
                for (const auto& action : entry.actions) {
                    writer(WebDashType::StorageWriteType::Append,
                        "    " + action + "\n");
                }
                writer(WebDashType::StorageWriteType::Append,
                    "fi\n\n");
                continue;
            }

            // Check if module has no destination path - use --install-module to read project-location
            if (entry.destination.empty() && !entry.source.empty()) {
                writer(WebDashType::StorageWriteType::Append,
                    "# Module with project-location: " + entry.source + "\n");
                string install_cmd = "webdash --install-module " + entry.source;
                if (entry.do_register) {
                    install_cmd += " --config.register";
                }
                install_cmd += "\n";
                writer(WebDashType::StorageWriteType::Append, install_cmd);

                // Run post-install task if specified (need to use internal command to discover location)
                if (!entry.webdash_task.empty()) {
                    writer(WebDashType::StorageWriteType::Append,
                        "# Note: post-install task '" + entry.webdash_task + "' requires manual execution after install\n");
                }
                writer(WebDashType::StorageWriteType::Append, "\n");
                continue;
            }

            // Build clone command with optional branch (git-based dependency with explicit path)
            string clone_cmd = "git clone";
            if (!entry.branch.empty()) {
                clone_cmd += " -b " + entry.branch;
            }
            clone_cmd += " " + entry.source + " " + entry.destination + " &> /dev/null\n";
            writer(WebDashType::StorageWriteType::Append, clone_cmd);

            // Run post-clone task if specified
            if (!entry.webdash_task.empty()) {
                writer(WebDashType::StorageWriteType::Append, "webdash " + entry.destination + "/" + WebDashCore::GetProjectConfigFilename() + entry.webdash_task + "\n");
            }

            if (entry.do_register) {
                writer(WebDashType::StorageWriteType::Append, "webdash --config.register " + entry.destination + "/" + WebDashCore::GetProjectConfigFilename() + "\n");
            }
        }

        writer(WebDashType::StorageWriteType::End, "");
    });

    return true;
}


/**
 * @brief Command: `webdash --profile.reapply`.
 *
 * Re-generates initialize-projects.sh from the current webdash.profile.jsonc
 * and executes it, applying any new modules or dependencies without running
 * the full setup-webdash.sh script.
 *
 * @param arguments The (command line) arguments.
 * @returns True if handled; false otherwise.
 */
bool ProfileReapply_Command(const vector<string>& arguments) {
    if (arguments.size() != 1)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::PROFILE_REAPPLY)
        return false;

    // Step 1: Regenerate initialize-projects.sh from webdash.profile.jsonc
    cout << "Regenerating project initializer from profile..." << endl;
    bool cloner_ok = CreateProjectCloner_InternalCommand({NATIVE_COMMANDS::_INT_CREATE_PROJECT_CLONER});
    if (!cloner_ok) {
        cerr << "ERROR: Failed to generate initialize-projects.sh" << endl;
        return true;
    }

    // Step 2: Execute the generated script
    string webdash_root = WebDashCore::Get().GetWebDashRootDirectory();
    string init_script = webdash_root + "/app-persistent/data/webdash-client/initialize-projects.sh";

    if (!filesystem::exists(init_script)) {
        cerr << "ERROR: Generated script not found at: " << init_script << endl;
        return true;
    }

    // Make executable and run
    filesystem::permissions(init_script,
        filesystem::perms::owner_exec | filesystem::perms::owner_read | filesystem::perms::owner_write,
        filesystem::perm_options::add);

    cout << "Running project initializer..." << endl;
    int ret = system(("bash \"" + init_script + "\"").c_str());

    if (ret != 0) {
        cerr << "WARNING: Some projects may have failed to initialize (exit code: " << ret << ")" << endl;
    } else {
        cout << "Profile re-applied successfully." << endl;
    }

    return true;
}


/**
 * @brief Command: `webdash --backup.create` / `webdash --backup.restore`.
 *
 * Runs backup.py from the webdash-file-backup project,
 * reading configuration from webdash.profile.jsonc's "backup" section.
 *
 * @param arguments The (command line) arguments.
 * @returns True if handled; false otherwise.
 */
bool Backup_Command(const vector<string>& arguments) {
    if (arguments.empty())
        return false;

    bool is_backup = (arguments[0] == NATIVE_COMMANDS::BACKUP_CREATE);
    bool is_restore = (arguments[0] == NATIVE_COMMANDS::BACKUP_RESTORE);

    if (!is_backup && !is_restore)
        return false;

    string webdash_root = WebDashCore::Get().GetWebDashRootDirectory();
    string backup_script = webdash_root + "/src/bin/_webdash/webdash-file-backup/backup.py";

    if (!filesystem::exists(backup_script)) {
        cerr << "ERROR: backup.py not found at: " << backup_script << endl;
        return true;
    }

    string action = is_backup ? "backup" : "restore";

    // Check for optional --dry-run flag
    bool dry_run = false;
    for (size_t i = 1; i < arguments.size(); ++i) {
        if (arguments[i] == "--dry-run") {
            dry_run = true;
        }
    }

    string cmd = "python3 \"" + backup_script + "\" " + action;
    if (dry_run) {
        cmd += " --dry-run";
    }

    cout << (is_backup ? "Starting backup..." : "Starting restore...") << endl;
    int ret = system(cmd.c_str());

    if (ret != 0) {
        cerr << "ERROR: " << action << " failed (exit code: " << ret << ")" << endl;
    }

    return true;
}


/**
 * @brief The set of recognized config-level (top-level) fields.
 *        Kept in sync with docs/configuration.md's "Full field reference".
 */
const std::set<string>& KnownConfigLevelFields() {
    static const std::set<string> fields = {
        "commands", "dependencies", "defaults", "vars", "env", "network-shared",
        "network-shared-sourcedir", "widget-identifier", "project-name",
        "project-location", "priority"
    };
    return fields;
}


/**
 * @brief The set of recognized command-level fields.
 *        Kept in sync with docs/configuration.md's "Full field reference".
 */
const std::set<string>& KnownCommandLevelFields() {
    static const std::set<string> fields = {
        "name", "action", "actions", "action-else", "actions-else",
        "dependencies", "wdir", "condition", "exit-actions", "executionStyle",
        "continue_on_error", "frequency", "when", "background", "user",
        "silent", "test-case", "notify-dashboard", "allow-execution-as-ancestor"
    };
    return fields;
}


/**
 * @brief Command: `webdash --config.validate [path-to-config]`.
 *
 * Lints a webdash.config.jsonc without running anything: flags unknown
 * top-level/command fields (usually typos), missing required fields, and a
 * few common mistakes (duplicate command names, else-branches without a
 * condition, unrecognized executionStyle values).
 *
 * @param arguments The (command line) arguments.
 * @returns Exit code: 0 if no issues (or only warnings), 1 if errors were found
 *          or the file could not be read/parsed, -1 if this isn't a `--config.validate`
 *          invocation at all (not handled).
 */
int ConfigValidate_Command(const vector<string>& arguments) {
    if (arguments.empty() || arguments[0] != NATIVE_COMMANDS::CONFIG_VALIDATE)
        return -1;

    if (arguments.size() > 2) {
        cerr << "Usage: webdash --config.validate [path-to-config]" << endl;
        return 1;
    }

    string config_path = (arguments.size() == 2)
        ? arguments[1]
        : (string("./") + WebDashCore::GetProjectConfigFilename());

    if (!fs::exists(config_path)) {
        cerr << "ERROR: Config file not found: " << config_path << endl;
        return 1;
    }

    json config;
    try {
        // Accept JSONC ("//"/"/* */" comments, trailing commas) - same as the
        // runtime parser, so --config.validate doesn't reject something webdash would
        // happily run.
        ifstream config_stream(config_path);
        config = json::parse(config_stream, nullptr, true, true, true);
    } catch (const std::exception& e) {
        cerr << "ERROR: Failed to parse '" << config_path << "' as JSON: " << e.what() << endl;
        return 1;
    }

    int errors = 0;
    int warnings = 0;

    auto report = [&](const string& message, bool is_error) {
        if (is_error) {
            cerr << "  [ERROR] " << message << endl;
            errors++;
        } else {
            cerr << "  [WARN]  " << message << endl;
            warnings++;
        }
    };

    cout << "Validating: " << config_path << endl;

    if (!config.is_object()) {
        report("Config must be a JSON object.", true);
        cerr << endl << errors << " error(s), " << warnings << " warning(s)." << endl;
        return 1;
    }

    for (auto it = config.begin(); it != config.end(); ++it) {
        if (KnownConfigLevelFields().find(it.key()) == KnownConfigLevelFields().end()) {
            report("Unknown top-level field '" + it.key() + "'.", false);
        }
    }

    if (!config.contains("commands") || !config["commands"].is_array()) {
        report("Missing or invalid required 'commands' array.", true);
    } else {
        std::set<string> seen_names;
        int index = 0;

        for (const auto& command : config["commands"]) {
            string label = "commands[" + to_string(index) + "]";

            if (!command.is_object()) {
                report(label + ": must be an object.", true);
                index++;
                continue;
            }

            if (command.contains("name") && command["name"].is_string()) {
                string name = command["name"].get<string>();
                label = "command '" + name + "'";
                if (!seen_names.insert(name).second) {
                    report("Duplicate " + label + ".", true);
                }
            } else {
                report(label + ": missing required 'name' field.", true);
            }

            if (!command.contains("action") && !command.contains("actions")) {
                report(label + ": needs an 'action' or 'actions' field.", true);
            }

            if (command.contains("action-else") && !command.contains("condition")) {
                report(label + ": 'action-else' has no effect without 'condition'.", false);
            }
            if (command.contains("actions-else") && !command.contains("condition")) {
                report(label + ": 'actions-else' has no effect without 'condition'.", false);
            }

            if (command.contains("executionStyle") && command["executionStyle"].is_string()) {
                string style = command["executionStyle"].get<string>();
                if (style != "stopOnFirstFailure" && style != "stopOnFirstSuccess" && style != "neverStop") {
                    report(label + ": unknown executionStyle '" + style + "'.", true);
                }
            }

            for (auto it = command.begin(); it != command.end(); ++it) {
                if (KnownCommandLevelFields().find(it.key()) == KnownCommandLevelFields().end()) {
                    report(label + ": unknown field '" + it.key() + "'.", false);
                }
            }

            index++;
        }
    }

    cout << endl;
    if (errors == 0 && warnings == 0) {
        cout << "OK: no issues found." << endl;
    } else {
        cout << errors << " error(s), " << warnings << " warning(s)." << endl;
    }

    return errors > 0 ? 1 : 0;
}


/**
 * @brief Registers the config from tracking by server. Supported command types:
 *
 *           `webdash --config.register`
 *                Attempts to register the file ./webdash.config.jsonc
 *
 *
 *           `webdash --config.register <path-to-config>`
 *                Attempts to register the file <path-to-config>
 *
 *           `webdash --config.register <path-to-config> --user <username>`
 *                Registers the config and runs tasks as the specified user
 *
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool ConfigRegister_Command(const vector<string>& arguments) {
    if (arguments.size() < 1 || arguments.size() > 4)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::CONFIG_REGISTER)
        return false;


    string path = string("./") + WebDashCore::GetProjectConfigFilename();
    string runAsUser = "";

    // Parse arguments
    for (size_t i = 1; i < arguments.size(); i++) {
        if (arguments[i] == "--user" && i + 1 < arguments.size()) {
            runAsUser = arguments[++i];
        } else if (path == string("./") + WebDashCore::GetProjectConfigFilename()) {
            // First non-option argument is the path
            path = arguments[i];
        }
    }

    auto wconfig = WebDashConfig(path);

    if (wconfig.LastLodingSucceeded()) {
        WebDashRegister(wconfig.GetPath(), runAsUser);
        if (!runAsUser.empty()) {
            cout << "Registered config to run as user: " << runAsUser << endl;
        }
        return true;
    }

    return false;
}


/**
 * @brief Unregisters the config from tracking by server. Supported command types:
 *
 *          `webdash --config.unregister`
 *                Attempts to unregister the file ./webdash.config.jsonc
 *
 *
 *          `webdash --config.unregister <path-to-config>`
 *                Attempts to unregister the file <path-to-config>
 *
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool ConfigUnregister_Command(const vector<string>& arguments) {
    if (arguments.size() != 1 && arguments.size() != 2)
        return false;

    if  (arguments[0] != NATIVE_COMMANDS::CONFIG_UNREGISTER)
        return false;

    string path = string("./") + WebDashCore::GetProjectConfigFilename();
    if (arguments.size() == 2) path = arguments[1];


    auto wconfig = WebDashConfig(path);

    if (wconfig.LastLodingSucceeded()) {
        // File exists - use the resolved path
        WebDashUnRegister(wconfig.GetPath());
        return true;
    }

    // File doesn't exist - try to unregister with the provided path anyway
    // (useful for cleaning up stale registrations)
    auto resolvedPath = std::filesystem::absolute(path).string();
    WebDashUnRegister(resolvedPath);
    return true;
}


/**
 * @brief Command: `webdash --config.reload-registered`.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool ConfigReloadRegistered_Command(const vector<string>& arguments) {
    if (arguments.size() != 1)
        return false;

    if  (arguments[0] != NATIVE_COMMANDS::CONFIG_RELOAD_REGISTERED)
        return false;

    WebDashReloadAll();

    return true;
}


/**
 * @brief Command: `webdash ping-server`.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool PingServer_Command(const vector<string>& arguments) {
    if (arguments.size() != 1)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::PING_SERVER)
        return false;

    WebDashPingServer();

    return true;
}


/**
 * @brief Command: `webdash --tools.ports.status <port>`.
 * Checks if a port is in use and shows which process is using it.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool ToolsPortsStatus_Command(const vector<string>& arguments) {
    if (arguments.size() != 2)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::TOOLS_PORTS_STATUS)
        return false;

    int port = 0;
    try {
        port = std::stoi(arguments[1]);
    } catch (...) {
        cerr << "Error: Invalid port number: " << arguments[1] << endl;
        return true;
    }

    if (port < 1 || port > 65535) {
        cerr << "Error: Port must be between 1 and 65535" << endl;
        return true;
    }

    // Use ss command to check port status
    string cmd = "ss -tlnp 'sport = :" + to_string(port) + "' 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        cerr << "Error: Failed to run ss command" << endl;
        return true;
    }

    string output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }
    pclose(pipe);

    if (output.find("LISTEN") == string::npos) {
        cout << "Port " << port << ": " << "\033[32mUNUSED\033[0m" << endl;
    } else {
        cout << "Port " << port << ": " << "\033[31mIN USE\033[0m" << endl;

        // Extract process info from ss output
        // Format: LISTEN  0  5  0.0.0.0:8089  0.0.0.0:*  users:(("python3",pid=12345,fd=3))
        size_t users_pos = output.find("users:((");
        if (users_pos != string::npos) {
            size_t start = users_pos + 8;  // Skip 'users:(("'
            size_t end = output.find("\"", start);
            if (end != string::npos) {
                string process_name = output.substr(start, end - start);
                cout << "  Process: " << process_name << endl;
            }

            // Extract PID
            size_t pid_pos = output.find("pid=", users_pos);
            if (pid_pos != string::npos) {
                size_t pid_start = pid_pos + 4;
                size_t pid_end = output.find_first_of(",)", pid_start);
                if (pid_end != string::npos) {
                    string pid = output.substr(pid_start, pid_end - pid_start);
                    cout << "  PID: " << pid << endl;

                    // Get the command line of the process
                    string cmdline_path = "/proc/" + pid + "/cmdline";
                    ifstream cmdline_file(cmdline_path);
                    if (cmdline_file.is_open()) {
                        string cmdline;
                        getline(cmdline_file, cmdline);
                        // Replace null bytes with spaces
                        for (char& c : cmdline) {
                            if (c == '\0') c = ' ';
                        }
                        if (!cmdline.empty()) {
                            cout << "  Command: " << cmdline << endl;
                        }
                    }

                    // Get the working directory
                    string cwd_path = "/proc/" + pid + "/cwd";
                    char cwd_buf[4096];
                    ssize_t len = readlink(cwd_path.c_str(), cwd_buf, sizeof(cwd_buf) - 1);
                    if (len != -1) {
                        cwd_buf[len] = '\0';
                        cout << "  Working Dir: " << cwd_buf << endl;
                    }
                }
            }
        }
    }

    return true;
}


/**
 * @brief Helper function to read project-location from a cloned repo's webdash.config.jsonc.
 * @param repo_path Path to the cloned repository.
 * @returns The project-location value if found, empty string otherwise.
 */
string ReadProjectLocation(const string& repo_path) {
    string config_path = repo_path + "/" + WebDashCore::GetProjectConfigFilename();

    if (!fs::exists(config_path)) {
        return "";
    }

    try {
        ifstream config_file(config_path);
        json config = json::parse(config_file, nullptr, true, true, true);

        if (config.contains(WebDashConst::Public::kModuleFieldProjectLocation)) {
            string location = config[WebDashConst::Public::kModuleFieldProjectLocation].get<string>();

            // Apply substitutions
            auto substitutions = WebDashCore::Get().GetPrimaryKeywordSubstitutions();
            location = WebDashUtils::ApplySubstitutions(location, substitutions);

            return location;
        }
    } catch (const std::exception& e) {
        cerr << "Warning: Failed to parse " << config_path << ": " << e.what() << endl;
    }

    return "";
}


/**
 * @brief Command: `webdash --install-module <git-url>`.
 *
 * Installs a module by:
 * 1. Cloning the repository to a temporary location
 * 2. Reading the project-location from webdash.config.jsonc
 * 3. Moving the repo to the specified destination
 * 4. Optionally registering with WebDash server
 *
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool InstallModule_Command(const vector<string>& arguments) {
    if (arguments.size() < 2 || arguments.size() > 3)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::INSTALL_MODULE)
        return false;

    string git_url = arguments[1];
    bool do_register = (arguments.size() == 3 && arguments[2] == "--config.register");

    cout << endl;
    cout << "Installing module from: " << git_url << endl;

    // Create temporary directory for cloning
    fs::path root_dir = WebDashCore::Get().GetWebDashRootDirectory();
    fs::path temp_base = root_dir / WebDashConst::Public::kTemporaryStorageDir / "module-install";
    fs::create_directories(temp_base);

    // Generate unique temp directory name from git URL
    string repo_name = git_url;
    // Extract repo name from URL (e.g., "repo.git" from "git@github.com:user/repo.git")
    size_t last_slash = repo_name.rfind('/');
    size_t last_colon = repo_name.rfind(':');
    size_t start = (last_slash != string::npos) ? last_slash + 1 :
                   (last_colon != string::npos) ? last_colon + 1 : 0;
    repo_name = repo_name.substr(start);
    // Remove .git suffix
    if (repo_name.size() > 4 && repo_name.substr(repo_name.size() - 4) == ".git") {
        repo_name = repo_name.substr(0, repo_name.size() - 4);
    }

    fs::path temp_path = temp_base / repo_name;

    // Clean up any previous temp clone
    if (fs::exists(temp_path)) {
        cout << "Removing existing temp directory: " << temp_path << endl;
        fs::remove_all(temp_path);
    }

    // Clone to temporary location
    cout << "Cloning to temporary location..." << endl;
    string clone_cmd = "git clone " + git_url + " " + temp_path.string() + " 2>&1";
    int clone_result = system(clone_cmd.c_str());

    if (clone_result != 0) {
        cerr << "ERROR: Failed to clone repository: " << git_url << endl;
        return true;  // Command was handled, but failed
    }

    // Read project-location from the cloned repo
    string project_location = ReadProjectLocation(temp_path.string());

    if (project_location.empty()) {
        cerr << "ERROR: No 'project-location' field found in " << WebDashCore::GetProjectConfigFilename() << endl;
        cerr << "The repository must have a webdash.config.jsonc with a 'project-location' field." << endl;
        cerr << "Example: { \"project-location\": \"$.rootDir()/src/bin/my-project\", ... }" << endl;
        // Clean up temp
        fs::remove_all(temp_path);
        return true;
    }

    cout << "Project location: " << project_location << endl;

    // Check if destination already exists
    if (fs::exists(project_location)) {
        cerr << "ERROR: Destination already exists: " << project_location << endl;
        cerr << "Remove it first if you want to reinstall." << endl;
        fs::remove_all(temp_path);
        return true;
    }

    // Create parent directories if needed
    fs::path dest_parent = fs::path(project_location).parent_path();
    if (!fs::exists(dest_parent)) {
        cout << "Creating directory: " << dest_parent << endl;
        fs::create_directories(dest_parent);
    }

    // Move from temp to destination
    cout << "Moving to: " << project_location << endl;
    try {
        fs::rename(temp_path, project_location);
    } catch (const fs::filesystem_error& e) {
        // rename can fail across filesystems, fall back to copy+delete
        cout << "Cross-filesystem move, copying instead..." << endl;
        fs::copy(temp_path, project_location, fs::copy_options::recursive);
        fs::remove_all(temp_path);
    }

    cout << "Module installed successfully!" << endl;

    // Register with WebDash server if requested
    if (do_register) {
        fs::path config_path = fs::path(project_location) / WebDashCore::GetProjectConfigFilename();
        if (fs::exists(config_path)) {
            cout << "Registering with WebDash server..." << endl;
            WebDashRegister(config_path.string());
        }
    }

    cout << endl;
    return true;
}


/**
 * @brief Command: `webdash --server-process-once <path>`.
 *        Tells the webdash-server to immediately process a config file (runs only tasks with frequency set).
 *        Useful for debugging scheduled task execution.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
/**
 * @brief Commands: `webdash --tools.ports.forwarded.list`, `webdash --tools.ports.forwarded.add <ports...>`,
 *        `webdash --tools.ports.forwarded.remove <ports...>`.
 * Listing is server-side (via IPC) so privileged checks can be performed without sudo prompts.
 * Add/remove currently dispatch to the local wsl-port-forward.py tool (Windows admin prompts are interactive).
 * @param arguments The (command line) arguments.
 * @returns True if handled; false otherwise.
 */
bool ToolsPortsForwarded_Command(const vector<string>& arguments) {
    if (arguments.empty())
        return false;

    string subcmd;
    bool needs_ports = false;

    if (arguments[0] == NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_LIST) {
        subcmd = "list";
    } else if (arguments[0] == NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_ADD) {
        subcmd = "forward";
        needs_ports = true;
    } else if (arguments[0] == NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_REMOVE) {
        subcmd = "reset";
        needs_ports = true;
    } else {
        return false;
    }

    if (needs_ports && arguments.size() < 2) {
        cerr << "Error: At least one port number is required." << endl;
        cerr << "Usage: webdash " << arguments[0] << " <port1> [port2] ..." << endl;
        return true;
    }

    fs::path script = fs::path(WebDashCore::Get().GetWebDashRootDirectory())
                      / "src" / "bin" / "_webdash" / "port-forward" / "wsl-port-forward.py";

    if (!fs::exists(script)) {
        cerr << "Error: Port forwarding script not found: " << script << endl;
        return true;
    }

    // List is server-only: the server may run as root under systemd, enabling iptables inspection
    // without prompting the interactive user for sudo.
    if (!needs_ports && subcmd == "list") {
        try {
            websocket_endpoint endpoint;
            int id = ConnectEndpoint(&endpoint);

            const auto& info = IpcRegistry::Get(IpcCommand::TOOLS_PORTS_FORWARDED_LIST);
            endpoint.sendAndWaitForResponse(id, info.wire, "*", [&](string resp) {
                cout << resp << endl;
            });

            return true;
        } catch (const std::exception& e) {
            cerr << "Error: webdash-server is not reachable (" << e.what() << ")" << endl;
            cerr << "This command is server-side. Start the server and try again." << endl;
            return true;
        } catch (...) {
            cerr << "Error: webdash-server is not reachable" << endl;
            cerr << "This command is server-side. Start the server and try again." << endl;
            return true;
        }
    }

    if (needs_ports) {
        // Validate all port arguments first
        vector<int> ports;
        for (size_t i = 1; i < arguments.size(); ++i) {
            try {
                int port = std::stoi(arguments[i]);
                if (port < 1 || port > 65535) {
                    cerr << "Error: Port must be between 1 and 65535: " << arguments[i] << endl;
                    return true;
                }
                ports.push_back(port);
            } catch (...) {
                cerr << "Error: Invalid port number: " << arguments[i] << endl;
                return true;
            }
        }

        for (int port : ports) {
            string cmd = "python3 " + script.string() + " " + subcmd + " " + to_string(port);
            system(cmd.c_str());
            if (ports.size() > 1) cout << endl;
        }
        return true;
    } else {
        string cmd = "python3 " + script.string() + " " + subcmd;
        system(cmd.c_str());
        return true;
    }
}


bool ServerProcessOnce_Command(const vector<string>& arguments) {
    if (arguments.size() != 2)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::SERVER_PROCESS_ONCE)
        return false;

    string config_path = arguments[1];

    // Resolve to absolute path if relative
    if (!fs::path(config_path).is_absolute()) {
        config_path = fs::absolute(config_path).string();
    }

    if (!fs::exists(config_path)) {
        cerr << "Error: Config file not found: " << config_path << endl;
        return true;
    }

    cout << "Requesting server to process: " << config_path << endl;

    try {
        WebDashServerProcessOnce(config_path);
    } catch (const std::exception& e) {
        cerr << "Error communicating with server: " << e.what() << endl;
        cerr << "Is the webdash-server running?" << endl;
        return true;
    }

    return true;
}


/**
 * @brief Description lines for a native command, shown by `webdash --help --verbose`.
 *        The first line is a summary; any further lines explain individual arguments.
 * @param cmd The command string (a NATIVE_COMMANDS::* value).
 * @returns The description lines, or an empty vector if none is defined.
 */
vector<string> NativeCommandDescription(const string& cmd) {
    static const std::map<string, vector<string>> descriptions = {
        { NATIVE_COMMANDS::CONFIG_REGISTER, {
            "Register a config for periodic execution by the server.",
            "path: defaults to ./webdash.config.jsonc if omitted.",
            "--user <name>: run the config's tasks as this Unix user instead of the server's own." } },
        { NATIVE_COMMANDS::CONFIG_UNREGISTER, {
            "Remove a config from the registry.",
            "path: defaults to ./webdash.config.jsonc if omitted." } },
        { NATIVE_COMMANDS::CONFIG_RELOAD_REGISTERED, {
            "Tell the server to reload every registered config." } },
        { NATIVE_COMMANDS::HELP, {
            "Show this help text.",
            "--verbose: also print these descriptions." } },
        { NATIVE_COMMANDS::LIST_CONFIG, {
            "Print all registered config paths." } },
        { NATIVE_COMMANDS::LIST_DEFINITIONS, {
            "Show $.var()/profile substitutions available to a config.",
            "path-to-config: defaults to the current directory's config." } },
        { NATIVE_COMMANDS::PING_SERVER, {
            "Check whether the server is running and responsive." } },
        { NATIVE_COMMANDS::INSTALL_MODULE, {
            "Clone a repo, read its project-location, and move it there.",
            "git-url: the repository to clone.",
            "--config.register: also register the config after installing." } },
        { NATIVE_COMMANDS::TOOLS_PORTS_STATUS, {
            "Show whether a port is in use, and by which process.",
            "port: the port number to check." } },
        { NATIVE_COMMANDS::SERVER_PROCESS_ONCE, {
            "Immediately run a config's scheduled (frequency) tasks.",
            "config-path: the config to process." } },
        { NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_LIST, {
            "List ports forwarded from WSL to the Windows host." } },
        { NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_ADD, {
            "Forward one or more ports from WSL to the Windows LAN.",
            "port1, port2, ...: the port numbers to forward." } },
        { NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_REMOVE, {
            "Remove WSL-to-Windows forwarding for one or more ports.",
            "port1, port2, ...: the port numbers to stop forwarding." } },
        { NATIVE_COMMANDS::PROFILE_REAPPLY, {
            "Re-clone/set up modules and dependencies from the profile." } },
        { NATIVE_COMMANDS::BACKUP_CREATE, {
            "Copy configured files/directories to the backup location.",
            "--dry-run: preview what would be backed up without copying." } },
        { NATIVE_COMMANDS::BACKUP_RESTORE, {
            "Restore files/directories from the backup location.",
            "--dry-run: preview what would be restored without copying." } },
        { NATIVE_COMMANDS::CONFIG_VALIDATE, {
            "Lint a config for unknown/missing fields.",
            "config-path: defaults to ./webdash.config.jsonc if omitted." } },
    };

    auto it = descriptions.find(cmd);
    return it != descriptions.end() ? it->second : vector<string>{};
}


/**
 * @brief Command: `webdash help` (also `webdash --help` or `webdash -help`).
 *        Add `--verbose` to also print a one-line description under each command.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool Help_Command(const vector<string>& arguments) {
    if (arguments.empty() || arguments.size() > 2)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::HELP &&
        arguments[0] != "-h")
        return false;

    if (arguments.size() == 2 && arguments[1] != "--verbose")
        return false;

    bool verbose = (arguments.size() == 2);

    cout << endl;
    cout << "WebDash - Project command runner and environment manager" << endl;
    cout << endl;
    cout << "USAGE:" << endl;
    cout << "    webdash <command>                  Run command from ./webdash.config.jsonc" << endl;
    cout << "    webdash /path/to/project:<command> Run command from specified project" << endl;
    cout << "    webdash <native-command>           Run a WebDash native command" << endl;
    cout << endl;

    vector<string> commands = WebDashNativeCommands();

    cout << "NATIVE COMMANDS:" << endl;

    for (auto cmd : commands) {
        if (IsInternalCommand(cmd) == false) {
            if (cmd == NATIVE_COMMANDS::HELP) {
                cout << string(kSpaceOutCommands, ' ') << cmd << ", -h [--verbose]" << endl;
            } else if (cmd == NATIVE_COMMANDS::CONFIG_REGISTER) {
                cout << string(kSpaceOutCommands, ' ') << cmd << " [path] [--user <name>]" << endl;
            } else if (cmd == NATIVE_COMMANDS::CONFIG_UNREGISTER) {
                cout << string(kSpaceOutCommands, ' ') << cmd << " [path]" << endl;
            } else if (cmd == NATIVE_COMMANDS::LIST_DEFINITIONS) {
                cout << string(kSpaceOutCommands, ' ') << cmd << " [path-to-config]" << endl;
            } else if (cmd == NATIVE_COMMANDS::INSTALL_MODULE) {
                cout << string(kSpaceOutCommands, ' ') << cmd << " <git-url> [--config.register]" << endl;
            } else if (cmd == NATIVE_COMMANDS::SERVER_PROCESS_ONCE) {
                cout << string(kSpaceOutCommands, ' ') << cmd << " <config-path>" << endl;
            } else if (cmd == NATIVE_COMMANDS::CONFIG_VALIDATE) {
                cout << string(kSpaceOutCommands, ' ') << cmd << " [config-path]" << endl;
            } else if (cmd == NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_ADD || cmd == NATIVE_COMMANDS::TOOLS_PORTS_FORWARDED_REMOVE) {
                cout << string(kSpaceOutCommands, ' ') << cmd << " <port1> [port2] ..." << endl;
            } else if (cmd == NATIVE_COMMANDS::TOOLS_PORTS_STATUS) {
                cout << string(kSpaceOutCommands, ' ') << cmd << " <port>" << endl;
            } else if (cmd == NATIVE_COMMANDS::BACKUP_CREATE || cmd == NATIVE_COMMANDS::BACKUP_RESTORE) {
                cout << string(kSpaceOutCommands, ' ') << cmd << " [--dry-run]" << endl;
            } else {
                cout << string(kSpaceOutCommands, ' ') << cmd << endl;
            }

            if (verbose) {
                vector<string> description = NativeCommandDescription(cmd);
                if (!description.empty()) {
                    cout << string(kSpaceOutCommands + 4, ' ') << "# " << description[0] << endl;
                    for (size_t i = 1; i < description.size(); i++) {
                        cout << string(kSpaceOutCommands + 8, ' ') << "- " << description[i] << endl;
                    }
                }

                cout << endl;
            }
        }
    }

    cout << endl;
    cout << "EXAMPLES:" << endl;
    cout << "    webdash --install-module git@github.com:user/repo.git" << endl;
    cout << "        Install a module by reading its project-location from webdash.config.jsonc" << endl;
    cout << endl;
    cout << "    webdash --profile.reapply" << endl;
    cout << "        Re-apply modules/dependencies from webdash.profile.jsonc" << endl;
    cout << endl;
    cout << "    webdash --server-process-once /path/to/webdash.config.jsonc" << endl;
    cout << "        Simulate server-style execution (only tasks with frequency)" << endl;
    cout << endl;
    cout << "    webdash --backup.create" << endl;
    cout << "        Backup files/dirs listed in webdash.profile.jsonc's backup section" << endl;
    cout << endl;
    cout << "    webdash --backup.restore" << endl;
    cout << "        Restore files/dirs from backup" << endl;
    cout << endl;
    cout << "    webdash --backup.create --dry-run" << endl;
    cout << "        Preview what would be backed up without copying" << endl;
    cout << endl;
    cout << "    webdash --config.validate" << endl;
    cout << "        Lint ./webdash.config.jsonc for unknown/missing fields" << endl;
    cout << endl;
    cout << "    webdash --config.validate /path/to/webdash.config.jsonc" << endl;
    cout << "        Lint a specific config file" << endl;
    cout << endl;
    cout << "    webdash --tools.ports.forwarded.add 8080" << endl;
    cout << "        Configure Windows/WSL port forwarding so 8080 is reachable from your LAN" << endl;
    cout << endl;
    cout << "Run 'webdash --list-registered-configs' to see all registered project configs." << endl;

    return true;
}


/**
 * @brief Lists definitions available in the given config.
 *
 *          `webdash list-definitions`
 *
 *          `webdash list-definitions <path-to-config>`
 *
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool ListDefinitions_Command(const vector<string>& arguments) {

    // Needs to have at least the WebDash command for listing definitions.
    if (arguments.size() != 1 && arguments.size() != 2)
        return false;
    if  (arguments[0] != NATIVE_COMMANDS::LIST_DEFINITIONS)
        return false;

    vector<SubstitutionPair> definitions;

    if (arguments.size() == 2) {
        // Path specified - get definitions from that config
        vector<string> adjusted_arguments(arguments.begin() + 1, arguments.end());
        auto config_and_command = GetConfigAndCommand(adjusted_arguments);

        if (!config_and_command)
            return false;

        definitions = std::get<0>(*config_and_command).GetProfileConfigSubtitutions();
    } else {
        // No path specified - try local config, fall back to global definitions
        auto config_and_command = GetConfigAndCommand({});

        if (config_and_command) {
            definitions = std::get<0>(*config_and_command).GetProfileConfigSubtitutions();
        } else {
            // No local config - show global definitions from root profile
            const auto& keychain_values = WebDashCore::Get().GetKeyValuesFromRootProfile();
            for (auto& keychain_value : keychain_values) {
                definitions.push_back(WebDashUtils::GenerateWebDashConfigSubstitution(keychain_value));
            }

            // Add primary substitutions
            auto primary_subs = WebDashCore::Get().GetPrimaryKeywordSubstitutions();
            definitions.insert(definitions.end(), primary_subs.begin(), primary_subs.end());

            // Add $.thisDir() for current directory
            definitions.push_back(make_pair(WebDashConst::Public::kSubstThisDir, filesystem::current_path().string()));
        }
    }

    size_t lwidth = 3, rwidth = 3;

    for (const auto& def: definitions)
    {
        lwidth = max(lwidth, def.first.size() + 3);
        rwidth = max(rwidth, def.second.size() + 3);
    }

    for (const auto& def: definitions)
        cout << std::left << setw(lwidth) << def.first << setw(rwidth) << def.second << endl;

    return true;
}


/**
 * @brief Execute a command specified in a WebDash JSON configuration file.
 * @param arguments The command arguments.
 * @param verbose Whether to show verbose execution output.
 * @returns 0 if command succeeded, positive error code if failed, -1 if not handled.
 */
int ConfigBased_Command(const vector<string>& arguments, bool verbose = false) {
    auto config_and_command = GetConfigAndCommand(arguments);

    if (!config_and_command)
        return -1;  // Not handled

    auto& [config, command, extra_args] = *config_and_command;
    WebDashType::RunConfig run_config;
    run_config.verbose = verbose;
    auto ret = config.Run(command, run_config, extra_args);
    if (!ret.empty()) {
        // Check if any task failed
        for (const auto& result : ret) {
            if (result.return_code != 0) {
                return result.return_code;
            }
        }
        return 0;  // All succeeded
    }

    return -1;  // Not handled
}


/**
 * @brief Takes a WebDash action based on the given arguments.
 * @param argc The number of arguments.
 * @param argv The arguments.
 * @returns Exit code (0 for success, non-zero for failure).
 */
int ExecuteUserInput(int argc, char **argv) {
    size_t cmd_argument_count = argc;
    char** cmd_argument_values = argv;

    /**
     * Miscellaneous information for the user.
     */

    // Root directory info removed for cleaner output
    // const auto root_directory = WebDashCore::Get().GetWebDashRootDirectory();
    // cout << "Root: " << root_directory << "/" << WebDashCore::GetRootProfileFilename() << endl;
    // cout << "-----------------" << endl;
    // cout.flush();

    /*
        // Prints arguments passed in
        cout << "> args[" << cmd_argument_count << "] = ";
        for (size_t i = 0; i < cmd_argument_count; ++i)
            cout << "\'" << cmd_argument_values[i] << "\' ";
        cout << endl;
        cout << "=======================================" << endl;
        cout.flush();
    */

    /**
     * Scope down the arguments to the ones that matter (i.e., exclude the 0-th).
     * Also extract webdash-specific flags.
     */

    vector<string> arguments;
    bool verbose = (getenv("WEBDASH_VERBOSE") != nullptr);  // Inherit from parent process

    for (size_t i = 1; i < cmd_argument_count; ++i) {
        string arg = cmd_argument_values[i];
        if (arg == "--webdash-verbose") {
            verbose = true;
            // Set env var so forked processes also know
            setenv("WEBDASH_VERBOSE", "1", 1);
        } else {
            arguments.emplace_back(arg);
        }
    }

    /**
     * Non-config commands.
     */

    if (ListRegistered_Command(arguments)) return 0;
    if (CreateBuildInitializer_InternalCommand(arguments)) return 0;
    if (CreateProjectCloner_InternalCommand(arguments)) return 0;

    {
        int validate_result = ConfigValidate_Command(arguments);
        if (validate_result >= 0) return validate_result;
    }

    if (ConfigRegister_Command(arguments)) return 0;
    if (ConfigUnregister_Command(arguments)) return 0;
    if (ConfigReloadRegistered_Command(arguments)) return 0;
    if (PingServer_Command(arguments)) return 0;
    if (ToolsPortsStatus_Command(arguments)) return 0;
    if (InstallModule_Command(arguments)) return 0;
    if (ServerProcessOnce_Command(arguments)) return 0;
    if (ToolsPortsForwarded_Command(arguments)) return 0;
    if (ProfileReapply_Command(arguments)) return 0;
    if (Backup_Command(arguments)) return 0;
    if (Help_Command(arguments)) return 0;

    /**
     * Check commands that allow ancestry-based config determination.
     */

    if (ListDefinitions_Command(arguments)) return 0;
    // The MOST important handler for the USER:
    int config_result = ConfigBased_Command(arguments, verbose);
    if (config_result >= 0) return config_result;

    /**
     * Still not handled? Explain to the USER that it was **not possible** to take an action.
     */

    cerr << endl;
    cerr << "No matching command found." << endl;

    auto configWithCommand = GetConfigAndCommand(arguments);

    if (configWithCommand.has_value()) {
        cerr << endl << string(kSpaceOutGroup, ' ') << "From the selected WebDash config at '" << std::get<0>(*configWithCommand).GetPath() << "':" << endl;
        vector<string> commands = WebDashList(std::get<0>(*configWithCommand).GetPath());

        for (auto cmd : commands) {
            cerr << string(kSpaceOutCommands, ' ') << cmd << endl;
        }
    } else {
        // Check if config file exists but failed to parse
        auto config_path = std::filesystem::current_path() / WebDashCore::GetProjectConfigFilename();
        if (std::filesystem::exists(config_path)) {
            // Config exists but parsing must have failed - error was already printed
            cerr << endl << string(kSpaceOutGroup, ' ') << "Fix the JSON syntax error in '" << WebDashCore::GetProjectConfigFilename() << "' and try again." << endl;
        } else {
            cerr << endl << string(kSpaceOutGroup, ' ') << "No '" << WebDashCore::GetProjectConfigFilename() << "' found in current directory." << endl;
            cerr << string(kSpaceOutGroup, ' ') << "Run 'webdash --list-registered-configs' to see registered projects." << endl;
        }
    }

    vector<string> commands = WebDashNativeCommands();

    cerr << endl << string(kSpaceOutGroup, ' ') << "WebDash native commands:" << endl;

    for (auto cmd : commands) {
        if (IsInternalCommand(cmd) == false) {
            cerr << string(kSpaceOutCommands, ' ') << cmd << endl;
        }
    }

    cerr << endl << string(kSpaceOutGroup, ' ') << "For more help: webdash --help" << endl;
    return 1;  // No matching command found
}


/**
 * @brief Main entry point.
 * @param argc Number of command line arguments.
 * @param argv The arguments.
 */
int main(int argc, char **argv) {

    try {
        return ExecuteUserInput(argc, argv);
    } catch (WebDashException::NoWebDashRootFound& e) {
        // Specific handling for missing root config - common user error
        cerr << "\n";
        cerr << "ERROR: WebDash root configuration not found." << endl;
        cerr << "\n";
        cerr << "DETAILS:" << endl;
        cerr << "  " << e.what() << endl;
        cerr << "\n";
        cerr << "HINTS:" << endl;
        cerr << "  1. Ensure '" << WebDashCore::GetRootProfileFilename() << "' exists in an ancestor directory" << endl;
        cerr << "  2. The file must contain: { \"webdash\": { \"rootDir\": \"this\" } }" << endl;
        cerr << "  3. Or set the WEBDASH environment variable to the root directory" << endl;
        cerr << "\n";
        return 1;
    } catch (WebDashException::FileNotFound& e) {
        // Specific handling for missing project config file
        cerr << "\n";
        cerr << "ERROR: Project configuration file not found." << endl;
        cerr << "\n";
        cerr << "DETAILS:" << endl;
        cerr << "  " << e.what() << endl;
        cerr << "\n";
        cerr << "HINTS:" << endl;
        cerr << "  1. Create a '" << WebDashCore::GetProjectConfigFilename() << "' in the current directory" << endl;
        cerr << "  2. Or specify the full path: webdash /path/to/project:" << endl;
        cerr << "  3. Run 'webdash --list-registered-configs' to see all registered configs" << endl;
        cerr << "\n";
        return 1;
    } catch (WebDashException::General& e) {
        cerr << "\n";
        cerr << "ERROR: WebDash encountered an exception." << endl;
        cerr << "MESSAGE: " << e.what() << endl;
        cerr << "\n";
        e.PrintBacktrace(cerr);
        return 1;
    } catch (std::exception& e) {
        cerr << "\n";
        cerr << "ERROR: WebDash client received an std::exception." << endl;
        cerr << "MESSAGE: " << e.what() << endl;
        cerr << "\n";
        return 1;
    } catch (...) {
        cerr << "\n";
        cerr << "ERROR: WebDash client does not handle the thrown exception." << endl;
        cerr << "\n";
        throw;
    }
}
