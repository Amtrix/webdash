// Self
#include "../common/websocket.h"
#include "../common/utils.hpp"

// WebDash
#include <webdash-config.hpp>
#include <webdash-core.hpp>
#include <webdash-exceptions.hpp>

// Standard
#include <cstdio>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <algorithm>

// External
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace std;
using json = nlohmann::json;


/* extern */ const string _WEBDASH_PROJECT_NAME_ = "webdash-client";
const string _WEBDASH_INTERNAL_CMD_PREFIX = "_internal_:";
const string _WEBDASH_TERMINAL_INIT_FILE_WARNING = "# Warning: This is an automatically generated file by the app-persistent/bin/webdash client. Auto generated. Don't modify.";


/**
 * Commands to inject into a bash script to print the state of the WebDash process.
 */
const std::string _WEBDASH_TERMINAL_INIT_FILE_APPENDIX = R"(# Footer

if pgrep -x "webdash-server" > /dev/null
then
    echo 'WebDash: Server (<running>)'
else
    echo 'WebDash: Server (<not running>)'
fi

)";


/**
 * Mapping of WebDash commands to their user-friendly string format.
 */
namespace NATIVE_COMMANDS
{
    string REGISTER = "register";
    string UNREGISTER = "unregister";
    string RELOADALL = "reload-all";
    string HELP = "help";
    string LIST_CONFIG = "list-config";
    string LIST_DEFINITIONS = "list-definitions";
    string _INT_CREATE_BUILD_INIT = _WEBDASH_INTERNAL_CMD_PREFIX + "create-build-init";
    string _INT_CREATE_PROJECT_CLONER = _WEBDASH_INTERNAL_CMD_PREFIX + "create-project-cloner";
    string PING_SERVER = "ping-server";
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
             NATIVE_COMMANDS::PING_SERVER };
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
 * @brief Command: `webdash list-config`.
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
 * @brief Command: `webdash register`.
 * @param arguments The (command line) arguments.
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool CreateProjectCloner_InternalCommand(const vector<string>& arguments) {
    if (arguments.size() != 1)
        return false;

    if (arguments[0] != NATIVE_COMMANDS::_INT_CREATE_PROJECT_CLONER)
        return false;

    /**
     * Based on $MYWORLD/definitions.json, create a file that creates a clean clone of all the listed repositories,
     * listed in the 'pull-projects' key.
     *
     * WARNING: This file deletes everything previously cloned.
     */

    auto entries = WebDashCore::Get().GetExternalGitProjects();

    WebDashCore::Get().WriteToAppStorage("initialize-projects.sh", [&](WebDashType::StoreWriteChannel writer) {
        writer(WebDashType::StorageWriteType::Clear, "");

        for (auto entry : entries) {
            writer(WebDashType::StorageWriteType::Append, "git clone " + entry.source + " " + entry.destination + " &> /dev/null\n");
            writer(WebDashType::StorageWriteType::Append, "webdash " + entry.destination + "/webdash.config.json" + entry.webdash_task + "\n");

            if (entry.do_register) {
                writer(WebDashType::StorageWriteType::Append, "webdash register " + entry.destination + "/webdash.config.json\n");
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


    string path = "./webdash.config.json";
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

    string path = "./webdash.config.json";
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

    vector<string> adjusted_arguments(arguments.begin() + 1, arguments.end());

    auto config_and_command = GetConfigAndCommand(adjusted_arguments);

    if (!config_and_command)
        return false;

    auto definitions = config_and_command->first.GetProfileConfigSubtitutions();
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
 * @returns True if, based on the taken action, further execution should be terminated; false otherwise.
 */
bool ConfigBased_Command(const vector<string>& arguments) {
    auto config_and_command = GetConfigAndCommand(arguments);

    if (!config_and_command)
        return false;

    auto ret = config_and_command->first.Run(config_and_command->second);
    if (!ret.empty())
        return true;

    return false;
}


/**
 * @brief Takes a WebDash action based on the given arguments.
 * @param argc The number of arguments.
 * @param argv The arguments.
 */
void ExecuteUserInput(int argc, char **argv) {
    size_t cmd_argument_count = argc;
    char** cmd_argument_values = argv;

    /**
     * Miscellaneous information for the user.
     */

    const auto root_directory = WebDashCore::Get().GetWebDashRootDirectory();
    cout << "WebDash: Client (root: " << root_directory << ")" << endl;
    cout.flush();

    cout << "> args[" << cmd_argument_count << "] = ";
    for (size_t i = 0; i < cmd_argument_count; ++i)
        cout << "\'" << cmd_argument_values[i] << "\' ";
    cout << endl;
    cout << "=======================================" << endl;
    cout.flush();

    /**
     * Scope down the arguments to the ones that matter (i.e., exclude the 0-th).
     */

    vector<string> arguments;
    for (size_t i = 1; i < cmd_argument_count; ++i) {
        arguments.emplace_back(cmd_argument_values[i]);
    }

    /**
     * Non-config commands.
     */

    if (ListRegistered_Command(arguments)) return;
    if (CreateBuildInitializer_InternalCommand(arguments)) return;
    if (CreateProjectCloner_InternalCommand(arguments)) return;
    if (Register_Command(arguments)) return;
    if (Unregister_Command(arguments)) return;
    if (ReloadAll_Command(arguments)) return;
    if (PingServer_Command(arguments)) return;

    /**
     * Check commands that allow ancestry-based config determination.
     */

    if (ListDefinitions_Command(arguments)) return;
    // The MOST important handler for the USER:
    if (ConfigBased_Command(arguments)) return;

    /**
     * Still not handled? Explain to the USER that it was **not possible** to take an action.
     */

    cout << "No matching config/command found! Please select one of the following:" << endl;

    constexpr int kSpaceOutCommands = 8;
    constexpr int kSpaceOutGroup = 4;

    auto configWithCommand = GetConfigAndCommand(arguments);

    if (configWithCommand.has_value()) {

        cout << endl << string(kSpaceOutGroup, ' ') << "From the selected WebDash config at '" << configWithCommand->first.GetPath() << "':" << endl;
        vector<string> commands = WebDashList(configWithCommand->first.GetPath());

        for (auto cmd : commands) {
            cout << string(kSpaceOutCommands, ' ') << cmd << endl;
        }
    }

    vector<string> commands = WebDashNativeCommands();

    cout << endl << string(kSpaceOutGroup, ' ') << "WebDash's list of native commands:" << endl;

    for (auto cmd : commands) {
        if (IsInternalCommand(cmd) == false) {
            cout << string(kSpaceOutCommands, ' ') << cmd << endl;
        }
    }
}


/**
 * @brief Main entry point.
 * @param argc Number of command line arguments.
 * @param argv The arguments.
 */
int main(int argc, char **argv) {

    try {
        ExecuteUserInput(argc, argv);
    } catch (WebDashException::General& e) {
        cout << "ERROR: WebDash client received an WebDashException::General." << endl;
        cout << "MESSAGE: " << e.what() << endl;
        e.PrintBacktrace(cout);
        return 1;
    } catch (std::exception& e) {
        cout << "ERROR: WebDash client received an std::exception." << endl;
        cout << "MESSAGE: " << e.what() << endl;
        return 1;
    } catch (...) {
        cout << "ERROR: WebDash client does not handle the thrown exception." << endl;
        throw;
    }

    return 0;
}
