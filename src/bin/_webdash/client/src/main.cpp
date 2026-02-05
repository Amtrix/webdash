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
    string REGISTER = "--register";
    string UNREGISTER = "--unregister";
    string RELOADALL = "--reload-all";
    string HELP = "--help";
    string LIST_CONFIG = "--list-registered-configs";
    string LIST_DEFINITIONS = "--list-definitions";
    string _INT_CREATE_BUILD_INIT = _WEBDASH_INTERNAL_CMD_PREFIX + "create-build-init";
    string _INT_CREATE_PROJECT_CLONER = _WEBDASH_INTERNAL_CMD_PREFIX + "create-project-cloner";
    string PING_SERVER = "--ping-server";
    string INSTALL_MODULE = "--install-module";
    string PORT_STATUS = "--port-status";
};


/*
 * List of all commands supported by WebDash.
 */
vector<string> WebDashNativeCommands()
{
    return { NATIVE_COMMANDS::REGISTER,
             NATIVE_COMMANDS::UNREGISTER,
             NATIVE_COMMANDS::RELOADALL,
             NATIVE_COMMANDS::HELP,
             NATIVE_COMMANDS::LIST_CONFIG,
             NATIVE_COMMANDS::LIST_DEFINITIONS,
             NATIVE_COMMANDS::_INT_CREATE_BUILD_INIT,
             NATIVE_COMMANDS::_INT_CREATE_PROJECT_CLONER,
             NATIVE_COMMANDS::PING_SERVER,
             NATIVE_COMMANDS::INSTALL_MODULE,
             NATIVE_COMMANDS::PORT_STATUS };
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
     * Based on webdash-profile.json, create a file that creates a clean clone of all the listed repositories,
     * listed in the 'modules' key.
     *
     * WARNING: This file deletes everything previously cloned.
     */

    auto entries = WebDashCore::Get().GetExternalGitProjects();

    WebDashCore::Get().WriteToAppStorage("initialize-projects.sh", [&](WebDashType::StoreWriteChannel writer) {
        writer(WebDashType::StorageWriteType::Clear, "");
        writer(WebDashType::StorageWriteType::Append, "#!/bin/bash\n");
        writer(WebDashType::StorageWriteType::Append, "# Auto-generated by webdash - initialize modules from webdash-profile.json\n\n");

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
                    install_cmd += " --register";
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
                writer(WebDashType::StorageWriteType::Append, "webdash --register " + entry.destination + "/" + WebDashCore::GetProjectConfigFilename() + "\n");
            }
        }

        writer(WebDashType::StorageWriteType::End, "");
    });

    return true;
}


/**
 * @brief Registers the config from tracking by server. Supported command types:
 *
 *           `webdash register`
 *                Attempts to register the file ./webdash.config.json
 *
 *
 *           `webdash register <path-to-config>`
 *                Attempts to register the file <path-to-config>
 *
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool Register_Command(const vector<string>& arguments) {
    if (arguments.size() != 1 && arguments.size() != 2)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::REGISTER)
        return false;


    string path = string("./") + WebDashCore::GetProjectConfigFilename();
    if (arguments.size() == 2) path = arguments[1];

    auto wconfig = WebDashConfig(path);

    if (wconfig.LastLodingSucceeded()) {
        WebDashRegister(wconfig.GetPath());
        return true;
    }

    return false;
}


/**
 * @brief Unregisters the config from tracking by server. Supported command types:
 *
 *          `webdash unregister`
 *                Attempts to unregister the file ./webdash.config.json
 *
 *
 *          `webdash unregister <path-to-config>`
 *                Attempts to unregister the file <path-to-config>
 *
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool Unregister_Command(const vector<string>& arguments) {
    if (arguments.size() != 1 && arguments.size() != 2)
        return false;

    if  (arguments[0] != NATIVE_COMMANDS::UNREGISTER)
        return false;

    string path = string("./") + WebDashCore::GetProjectConfigFilename();
    if (arguments.size() == 2) path = arguments[1];

    auto wconfig = WebDashConfig(path);

    if (wconfig.LastLodingSucceeded()) {
        WebDashUnRegister(wconfig.GetPath());
        return true;
    }

    return false;
}


/**
 * @brief Command: `webdash reload-all`.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool ReloadAll_Command(const vector<string>& arguments) {
    if (arguments.size() != 1)
        return false;

    if  (arguments[0] != NATIVE_COMMANDS::RELOADALL)
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
 * @brief Command: `webdash --port-status <port>`.
 * Checks if a port is in use and shows which process is using it.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool PortStatus_Command(const vector<string>& arguments) {
    if (arguments.size() != 2)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::PORT_STATUS)
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
    int status = pclose(pipe);

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
 * @brief Helper function to read project-location from a cloned repo's webdash.config.json.
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
        json config = json::parse(config_file);

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
 * 2. Reading the project-location from webdash.config.json
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
    bool do_register = (arguments.size() == 3 && arguments[2] == "--register");

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
        cerr << "The repository must have a webdash.config.json with a 'project-location' field." << endl;
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
 * @brief Command: `webdash help` (also `webdash --help` or `webdash -help`).
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool Help_Command(const vector<string>& arguments) {
    if (arguments.size() != 1)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::HELP &&
        arguments[0] != "-h")
        return false;

    cout << endl;
    cout << "WebDash - Project command runner and environment manager" << endl;
    cout << endl;
    cout << "USAGE:" << endl;
    cout << "    webdash <command>                  Run command from ./webdash.config.json" << endl;
    cout << "    webdash /path/to/project:<command> Run command from specified project" << endl;
    cout << "    webdash <native-command>           Run a WebDash native command" << endl;
    cout << endl;

    vector<string> commands = WebDashNativeCommands();

    cout << "NATIVE COMMANDS:" << endl;

    for (auto cmd : commands) {
        if (IsInternalCommand(cmd) == false) {
            if (cmd == NATIVE_COMMANDS::HELP) {
                cout << string(kSpaceOutCommands, ' ') << cmd << ", -h" << endl;
            } else if (cmd == NATIVE_COMMANDS::INSTALL_MODULE) {
                cout << string(kSpaceOutCommands, ' ') << cmd << " <git-url> [--register]" << endl;
            } else {
                cout << string(kSpaceOutCommands, ' ') << cmd << endl;
            }
        }
    }

    cout << endl;
    cout << "EXAMPLES:" << endl;
    cout << "    webdash --install-module git@github.com:user/repo.git" << endl;
    cout << "        Install a module by reading its project-location from webdash.config.json" << endl;
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

    const auto root_directory = WebDashCore::Get().GetWebDashRootDirectory();
    cout << "Root: " << root_directory << "/" << WebDashCore::GetRootProfileFilename() << endl;
    cout << "-----------------" << endl;
    cout.flush();

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
    if (Register_Command(arguments)) return 0;
    if (Unregister_Command(arguments)) return 0;
    if (ReloadAll_Command(arguments)) return 0;
    if (PingServer_Command(arguments)) return 0;
    if (PortStatus_Command(arguments)) return 0;
    if (InstallModule_Command(arguments)) return 0;
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
