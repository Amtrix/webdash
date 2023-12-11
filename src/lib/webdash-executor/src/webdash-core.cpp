#include "webdash-utils.hpp"
#include "webdash-core.hpp"
#include "webdash-exceptions.hpp"

#include <nlohmann/json.hpp>

#include <queue>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>

using namespace std;
using json = nlohmann::json;


namespace {

    // Environment name under whose value the path to the WebDash's root directory is stored.
    constexpr char kWebDashRootEnvVarName[] = "WEBDASH";


    /**
     *  @brief Check the environment variables for the key kWebDashRootEnvVarName
     *         and return its value if it exists. If not, return nullopt.
     *
     *  @returnsThe value of the environment variable with name
     *          kWebDashRootEnvVarName.
    **/
    std::optional<string> TryGetRootUsingEnvironmentVariable() {
        std::optional<string> myworld_path = nullopt;
        const char* myworld_path_c = nullptr;

#ifdef _MSC_VER
        size_t myworld_path_len = 0;
        if (_dupenv_s(&myworld_path_c, &myworld_path_len, kWebDashRootEnvVarName) == 0 && myworld_path_c != nullptr)
#elif _PLATFORM_LINUX
        myworld_path_c = getenv(kWebDashRootEnvVarName);

        if (myworld_path_c != nullptr)
#endif
        {
            myworld_path = myworld_path_c;
        }

        return myworld_path;
    }

} // namespace


WebDashCore::WebDashCore(PrivateCtorClass private_ctor) {
    /* unused */ (void) private_ctor;

    // Determine the WebDash root directory (where the WebDash Profile JSON is located).
    _CalculateRootDirectory();
}


/* static */ WebDashCore& WebDashCore::Get() {
    if (_instance_creation_is_ongoing) {
        throw WebDashException::General("Unable to use ::Get() while the creation of the WebDashCore instance is in progress.");
    }

    // Instantiate the singleton if not already done so.
    if (!_singleton_instance.has_value()) {
        _instance_creation_is_ongoing = true;
        _singleton_instance.emplace(PrivateCtorClass{});
        _instance_creation_is_ongoing = false;

        assert(_singleton_instance.has_value());

        // Clear all log files.
        _singleton_instance->_InitializeLogFiles();

        // Success. WebDash's logging mechanism is possible at this point.
        _singleton_instance->Log(WebDashType::LogType::DEBUG, "WebDash successfully initialized with root path: " + _singleton_instance->_webdash_root_directory.string());
    }

    return _singleton_instance.value();
}


vector<pair<string, string>> WebDashCore::GetPrimaryKeywordSubstitutions() const {

    if (_webdash_root_directory.empty()) {
        throw WebDashException::General("WebDash's root directory not set.");
    }

    vector<pair<string,string>> substitutions;

    substitutions.push_back(make_pair("$.rootDir()", _webdash_root_directory.string()));

    return substitutions;
}


const vector<WebDashUtils::JsonEntry>& WebDashCore::GetKeyValuesFromRootProfile() const {
    return _profile_key_values;
}


void WebDashCore::_FinalizeInitialization(filesystem::path profile_filepath,
        vector<WebDashUtils::JsonEntry> key_values) {

    _webdash_root_directory = std::move(profile_filepath.parent_path());

    const auto keyword_substitutions = GetPrimaryKeywordSubstitutions();
    for (auto& key_value : key_values) {
        key_value.ApplySubstitutionsInValue(keyword_substitutions);
    }

    _profile_key_values = std::move(key_values);
}


void WebDashCore::_CalculateRootDirectory() {

    string last_json_parse_error_message;
    filesystem::path last_failed_json_parsing_path;

    /**
     * Checks the given path to a webdash-profile.json file for being a WebDash
     * root and parses its values.
     */
    auto CheckProfilePath = [&](const filesystem::path& profile_filepath) -> bool {
        vector<WebDashUtils::JsonEntry> key_values;

        /**
         * We allow JSON parsing failures but log the last one encountered. The
         * user could have modified the file and left it in a non-well formatted
         * JSON file.
         */
        try {
            key_values = WebDashUtils::ParseJSON(profile_filepath);
        } catch (const json::parse_error& parse_exception) {
            last_failed_json_parsing_path = profile_filepath;
            last_json_parse_error_message = parse_exception.what();
            return false;
        }

        filesystem::path filepath_directory = profile_filepath;
        filepath_directory.remove_filename();

        for (const auto& key_value : key_values) {

            /**
             * The ${_webdash_root_directory}/webdash-profile.json file must
             * define a magic key-value entry that adds an extra safeguard that
             * the right file was found.
             */
            if (WebDashUtils::GetWebDashJsonKey(key_value) == kMagicWebDashKeyInProfile &&
                    key_value.GetValue() == kMagicWebDashValueInProfile) {

                _FinalizeInitialization(std::move(profile_filepath), std::move(key_values));
                return true;
            }
        }

        return false;
    };

    filesystem::path starting_directory = filesystem::current_path();
    filesystem::path current_directory = starting_directory;

    //
    // There seems to be some weirdness going on when the drive is
    // network-mounted with vfs. fs::current_path() would return the root (/)
    // directory. In that case, we fallback to MYWORLD environment variable.
    //

    char *env_myworld = getenv("MYWORLD");

    if (strlen(env_myworld) > ((string)current_directory).size()) {
        current_directory = env_myworld;
    }

    Log(WebDashType::LogType::DEBUG, "For finding webdash-profile.json, the search starts with the following directory and goes upwards: " + starting_directory.string());

    std::optional<filesystem::path> next_directory = nullopt;

    // Starting with the CWD, go through the whole ancestry chain of directories.
    while (current_directory != next_directory) {

        /**
         * First directory is a special-case. The loop starts with the
         * assumption that there is no parent directory.
         */
        if (next_directory) {
            current_directory = next_directory.value();
        }

        next_directory = current_directory.parent_path();

        /**
         * Construct the full filepath to the webdash-profile.json file in the
         * current directory.
         */
        filesystem::path profile_filepath = current_directory;
        profile_filepath += string("/") + kRootProfileFilename;

        // Skip directory if it doesn't have the file.
        if (!std::filesystem::exists(profile_filepath)) {
            continue;
        }

        // Parse and verify the found file. Exit if its content is correct.
        if (CheckProfilePath(profile_filepath)) {
            return;
        }
    }

    // Fallback to checking for the MYWORLD environment variable.
    const auto myworld_directory_path_from_env_var =
        TryGetRootUsingEnvironmentVariable();

    if (myworld_directory_path_from_env_var.has_value() &&
            CheckProfilePath(myworld_directory_path_from_env_var.value()
                + "/" + kRootProfileFilename)) {
        return;
    }

    throw WebDashException::NoWebDashRootFound(
            "When traversing through all the ancestor directories, starting from '" + starting_directory.string() +
            "', WebDash was not able to find 'webdash-profile.json' with the JSON entry { '" + kMagicKeyInProfileErrorMessage +
            "' : '" + kMagicWebDashValueInProfile + "' }.",
        last_failed_json_parsing_path, last_json_parse_error_message);
}


void WebDashCore::_InitializeLogFiles() {
    Log(WebDashType::LogType::ERR, "");
    Log(WebDashType::LogType::INFO, "");
    Log(WebDashType::LogType::WARN, "");
    Log(WebDashType::LogType::DEBUG, "");
}


const filesystem::path& WebDashCore::GetWebDashRootDirectory() const {
    return _webdash_root_directory;
}


std::filesystem::path WebDashCore::GetPersistenteAppStoragePath() const {
    filesystem::path app_storage_path = GetWebDashRootDirectory();
    app_storage_path += string("/app-persistent/data/") + _WEBDASH_PROJECT_NAME_;
    std::filesystem::create_directories(app_storage_path);

    return app_storage_path;
}


void WebDashCore::WriteToAppStorage(
    const string filename,
    std::function<void(WebDashType::StoreWriteChannel)> callback)
{
    bool finished = false;

    // Get full path to destination. Directory is created.
    const string persistent_file = GetPersistenteAppStoragePath().string() + ("/" + filename);

    ofstream output_stream;
    output_stream.open(persistent_file, std::ofstream::out | std::ofstream::app);

    WebDashType::StoreWriteChannel writer = [&](WebDashType::StorageWriteType type, const string data) {
        if (type == WebDashType::StorageWriteType::End) {
            finished = true;
            return;
        } else if (type == WebDashType::StorageWriteType::Clear) {
            output_stream.close();
            output_stream.open(persistent_file, std::ofstream::out);
            return;
        } else if (type == WebDashType::StorageWriteType::Append) {
            output_stream << data;
        }
    };

    while (!finished) {
        callback(writer);
    }

    if (output_stream.is_open()) {
        output_stream.close();
    }
}


void WebDashCore::LoadFromAppStorage(const string& filename,
                                     const WebDashType::StorageReadType& type,
                                     std::function<void(istream&)> callback)
{

    const auto project_normalized_path = GetPersistenteAppStoragePath().string()
        + ("/" + filename);

    try {
        ifstream infilestream;
        infilestream.open(project_normalized_path.c_str(), ifstream::in);
        callback(infilestream);
    } catch (...) {
        Log(WebDashType::LogType::DEBUG, "Issues opening " + project_normalized_path + ". Not saved yet? Fallback to default (empty) return value.");

        stringstream input_stream;
        switch (type) {
            case WebDashType::StorageReadType::JSON:
                input_stream.str("{}");
                break;
            default:
                input_stream.str("");
                break;
        }

        callback(input_stream);
        return;
    }
}


void WebDashCore::Log(const WebDashType::LogType type,
                      const std::string msg,
                      const bool keep_version_from_previous_execution) {
    // Stores the current time for timestamping the log entries.
    std::string curr_time = "";

    // Computes the current time.
    {
        auto now_c = std::chrono::system_clock::now();
        std::time_t now_t = std::chrono::system_clock::to_time_t(now_c);

        std::stringstream ss;
        ss << std::put_time(std::localtime(&now_t), "%F %T");
        curr_time = ss.str();
    }

    filesystem::path full_log_file_path;
    
    /*
     * If webdash.config.json hasn't been determined yet, default to
     * app-temporary/webdash.LOG/DEBUG/INFO.txt 
    */

    if (_singleton_instance) {
        full_log_file_path = _GetAndCreateLogDirectory();

        full_log_file_path += "/logging."
                              + WebDashType::kLogTypeToString.at(type)
                              + ".txt";
    } else {
        full_log_file_path = getenv("MYWORLD");

        full_log_file_path += "/app-temporary/webdash."
                              + WebDashType::kLogTypeToString.at(type)
                              + ".txt";
    }

    const bool already_exists =
        std::filesystem::exists(full_log_file_path.c_str());

    const bool clear_previous_content = !_logfile_was_cleared[type] && !keep_version_from_previous_execution;

    // Check if the file needs be created or cleaned up (remove previous content due to cold boot of the program).
    if (!already_exists || clear_previous_content) {
        std::ofstream out(full_log_file_path.c_str());
        out << curr_time << ": File initialized." << std::endl;
        out.close();
        _logfile_was_cleared[type] = true;
    }

    // Write out the log statement.
    std::ofstream out(full_log_file_path.c_str(), std::ofstream::out | std::ofstream::app);
    out << curr_time << ": " << msg << std::endl;
    out.close();
}


void WebDashCore::Notify(const std::string msg) {
    Log(WebDashType::LogType::NOTIFY, msg, true);
}


vector<string> WebDashCore::GetEnvPathAdditions() {
    vector<string> ret;

    const auto defs = GetKeyValuesFromRootProfile();

    for (auto json_entry : defs) {
        if (json_entry.GetRootKey() != "path-add") {
            continue;
        }

        const string value = WebDashUtils::ApplySubstitutions(
            json_entry.GetValue(),
            GetPrimaryKeywordSubstitutions());

        ret.push_back(value);
    }

    return ret;
}


vector<pair<string, string>> WebDashCore::GetEnvironmentAdditions() {
    vector<pair<string, string>> ret;

    auto defs = GetKeyValuesFromRootProfile();

    for (auto json_entry : defs) {
        if (json_entry.GetRootKey() != "env") {
            continue;
        }

        const vector<string> tokens = json_entry.GetTokens();

        const string enironment_variable_name = json_entry.GetSuffixKeyPath(1);

        if (enironment_variable_name.size() == 0) {
            continue;
        }

        const string value = WebDashUtils::ApplySubstitutions(json_entry.GetValue(), GetPrimaryKeywordSubstitutions());

        ret.push_back({enironment_variable_name, value});
    }

    return ret;
}


vector<WebDashType::GitProjectMetadata> WebDashCore::GetExternalGitProjects() {
    unordered_map<string, WebDashType::GitProjectMetadata> projects;

    auto key_values_from_profile = GetKeyValuesFromRootProfile();

    for (const auto& json_entry : key_values_from_profile) {
        if (json_entry.GetRootKey() != "pull-projects") {
            continue;
        }

        const vector<string> tokens = json_entry.GetTokens();

        if (tokens.size() != 3) {
            continue;
        }

        const string array_index = tokens[1];
        const string property_name = tokens[2];

        const string value = WebDashUtils::ApplySubstitutions(json_entry.GetValue(), GetPrimaryKeywordSubstitutions());

        if (property_name == "source") {
            projects[array_index].source = value;
        }

        if (property_name == "destination") {
            projects[array_index].destination = value;
        }

        if (property_name == "exec") {
            projects[array_index].webdash_task = value;
        }

        if (property_name == "register") {
            projects[array_index].do_register = (value == "true");
        }
    }

    vector<WebDashType::GitProjectMetadata> ret;

    // Summarize all projects into a vector, each element representing one GIT project.
    for (const auto& element : projects) {
        ret.push_back(element.second);
    }

    return ret;
}


filesystem::path WebDashCore::_GetAndCreateLogDirectory() {
    const auto& myworld_path = GetWebDashRootDirectory();

    filesystem::path log_filepath = myworld_path;
    log_filepath += "/app-temporary/logging/";
    log_filepath += _WEBDASH_PROJECT_NAME_;

    std::filesystem::create_directories(log_filepath);

    return log_filepath;
}


WebDashCore& WebDash() {
    return WebDashCore::Get();
}


std::optional<WebDashCore> WebDashCore::_singleton_instance = nullopt;

bool WebDashCore::_instance_creation_is_ongoing = false;
