#include "webdash-utils.hpp"
#include "webdash-core.hpp"
#include "webdash-constants.hpp"
#include "webdash-exceptions.hpp"

#include <nlohmann/json.hpp>

#include <queue>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <map>

using namespace std;
using json = nlohmann::json;


namespace {

    /**
     *  @brief Check the environment variables for the key WebDashConst::Public::kEnvVarName
     *         and return its value if it exists. If not, return nullopt.
     *
     *  @returnsThe value of the environment variable with name
     *          WebDashConst::Public::kEnvVarName.
    **/
    std::optional<string> TryGetRootUsingEnvironmentVariable() {
        std::optional<string> webdash_root_path = nullopt;
        const char* webdash_root_path_c = nullptr;

#ifdef _MSC_VER
        size_t webdash_root_path_len = 0;
        if (_dupenv_s(&webdash_root_path_c, &webdash_root_path_len, WebDashConst::Public::kEnvVarName) == 0 && webdash_root_path_c != nullptr)
#elif _PLATFORM_LINUX
        webdash_root_path_c = getenv(WebDashConst::Public::kEnvVarName);

        if (webdash_root_path_c != nullptr)
#endif
        {
            webdash_root_path = webdash_root_path_c;
        }

        return webdash_root_path;
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

    substitutions.push_back(make_pair(WebDashConst::Public::kSubstRootDir, _webdash_root_directory.string()));

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
     * Checks the given path to a kRootProfileFilename file for being a WebDash
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
             * The ${_webdash_root_directory}/${kRootProfileFilename} file must
             * define a magic key-value entry that adds an extra safeguard that
             * the right file was found.
             */
            if (WebDashUtils::GetWebDashJsonKey(key_value) == WebDashConst::Internal::kMagicProfileKey &&
                    key_value.GetValue() == WebDashConst::Internal::kMagicProfileValue) {

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
    // directory. In that case, we fallback to WEBDASH environment variable.
    //

    const char *env_webdash_root = getenv(WebDashConst::Public::kEnvVarName);

    if (env_webdash_root != nullptr &&
        strlen(env_webdash_root) > current_directory.string().size()) {
        current_directory = env_webdash_root;
    }

    Log(WebDashType::LogType::DEBUG, string("For finding ") + WebDashConst::Public::kRootProfileFilename + ", the search starts with the following directory and goes upwards: " + starting_directory.string());

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
         * Construct the full filepath to the kRootProfileFilename file in the
         * current directory.
         */
        filesystem::path profile_filepath = current_directory;
        profile_filepath += string("/") + WebDashConst::Public::kRootProfileFilename;

        // Skip directory if it doesn't have the file.
        if (!std::filesystem::exists(profile_filepath)) {
            continue;
        }

        // Parse and verify the found file. Exit if its content is correct.
        if (CheckProfilePath(profile_filepath)) {
            return;
        }
    }

    // Fallback to checking for the WEBDASH environment variable.
    const auto webdash_directory_path_from_env_var =
        TryGetRootUsingEnvironmentVariable();

    if (webdash_directory_path_from_env_var.has_value() &&
            CheckProfilePath(webdash_directory_path_from_env_var.value()
                + "/" + WebDashConst::Public::kRootProfileFilename)) {
        return;
    }

    throw WebDashException::NoWebDashRootFound(
            "When traversing through all the ancestor directories, starting from '" + starting_directory.string() +
            "', WebDash was not able to find '" + WebDashConst::Public::kRootProfileFilename + "' with the JSON entry { '" + WebDashConst::Internal::kMagicProfileKeyHumanReadable +
            "' : '" + WebDashConst::Internal::kMagicProfileValue + "' }.",
        last_failed_json_parsing_path, last_json_parse_error_message);
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


// Static storage for persistent log file handles
namespace {
    std::map<WebDashType::LogType, std::ofstream> g_log_files;
    std::map<WebDashType::LogType, filesystem::path> g_log_paths;
    filesystem::path g_log_directory;
    bool g_log_files_initialized = false;
    std::mutex g_log_mutex;
    constexpr std::uintmax_t kMaxLogFileSize = 10 * 1024 * 1024;  // 10 MB

    std::string GetLogTimestamp() {
        auto now_c = std::chrono::system_clock::now();
        std::time_t now_t = std::chrono::system_clock::to_time_t(now_c);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&now_t), "%F %T");
        return ss.str();
    }

    void CheckAndRotateLog(WebDashType::LogType type) {
        auto it = g_log_paths.find(type);
        if (it == g_log_paths.end() || !filesystem::exists(it->second)) {
            return;
        }

        std::error_code ec;
        auto file_size = filesystem::file_size(it->second, ec);
        if (ec || file_size < kMaxLogFileSize) {
            return;
        }

        // Close file if open
        auto file_it = g_log_files.find(type);
        if (file_it != g_log_files.end() && file_it->second.is_open()) {
            file_it->second.close();
        }

        // Rotate
        filesystem::path previous = it->second.parent_path() /
            (WebDashConst::Public::kLogRotationPrefix + it->second.filename().string());
        filesystem::remove(previous, ec);
        filesystem::rename(it->second, previous, ec);

        // Reopen
        g_log_files[type].open(it->second, std::ios::out | std::ios::app);
        if (g_log_files[type].is_open()) {
            g_log_files[type] << GetLogTimestamp() << ": Log rotated (previous file exceeded "
                              << (kMaxLogFileSize / (1024 * 1024)) << " MB)." << std::endl;
        }
    }

    void InitLogFiles(const filesystem::path& log_directory) {
        if (g_log_files_initialized && g_log_directory == log_directory) {
            return;
        }

        // Close any existing files
        for (auto& [type, file] : g_log_files) {
            if (file.is_open()) {
                file.close();
            }
        }
        g_log_files.clear();
        g_log_paths.clear();

        g_log_directory = log_directory;
        filesystem::create_directories(log_directory);

        const std::vector<WebDashType::LogType> types = {
            WebDashType::LogType::DEBUG,
            WebDashType::LogType::INFO,
            WebDashType::LogType::WARN,
            WebDashType::LogType::ERR,
            WebDashType::LogType::NOTIFY
        };

        for (auto type : types) {
            auto path = log_directory / (string(WebDashConst::Public::kLogFilenamePrefix)
                        + WebDashType::kLogTypeToString.at(type) + ".txt");
            g_log_paths[type] = path;
            CheckAndRotateLog(type);
            g_log_files[type].open(path, std::ios::out | std::ios::app);
        }

        g_log_files_initialized = true;
    }

    // Cleanup on program exit
    struct LogCleanup {
        ~LogCleanup() {
            std::lock_guard<std::mutex> lock(g_log_mutex);
            for (auto& [type, file] : g_log_files) {
                if (file.is_open()) {
                    file.flush();
                    file.close();
                }
            }
        }
    };
    static LogCleanup g_log_cleanup;
}


void WebDashCore::Log(const WebDashType::LogType type,
                      const std::string msg) {
    std::lock_guard<std::mutex> lock(g_log_mutex);

    filesystem::path log_directory;

    if (_singleton_instance) {
        log_directory = _GetAndCreateLogDirectory();
    } else {
        const char* env_root = getenv(WebDashConst::Public::kEnvVarName);
        if (env_root == nullptr) {
            return;  // Can't log before root is found
        }
        log_directory = filesystem::path(env_root) / WebDashConst::Public::kTemporaryStorageDir;
    }

    // Initialize or reinitialize log files if directory changed
    if (!g_log_files_initialized || g_log_directory != log_directory) {
        InitLogFiles(log_directory);
    }

    auto it = g_log_files.find(type);
    if (it == g_log_files.end() || !it->second.is_open()) {
        return;
    }

    it->second << GetLogTimestamp() << ": " << msg << std::endl;
}


void WebDashCore::Notify(const std::string msg) {
    Log(WebDashType::LogType::NOTIFY, msg);
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
    vector<WebDashType::GitProjectMetadata> bundle_projects;  // For bundle entries

    auto key_values_from_profile = GetKeyValuesFromRootProfile();

    for (const auto& json_entry : key_values_from_profile) {
        const string& root_key = json_entry.GetRootKey();

        // Parse both "modules" and "dependencies" arrays
        if (root_key != WebDashConst::Public::kProfileKeyModules &&
            root_key != WebDashConst::Public::kProfileKeyDependencies) {
            continue;
        }

        const vector<string> tokens = json_entry.GetTokens();

        // Handle both simple fields (3 tokens: dependencies.0.git)
        // and array fields (4 tokens: dependencies.0.actions.0 or modules.0.bundle.0)
        if (tokens.size() < 3) {
            continue;
        }

        // Use a composite key to distinguish modules from dependencies with same index
        const string array_index = root_key + "." + tokens[1];
        const string property_name = tokens[2];

        const string value = WebDashUtils::ApplySubstitutions(json_entry.GetValue(), GetPrimaryKeywordSubstitutions());

        // Handle bundle array (4 tokens: modules.0.bundle.0)
        // Each entry in bundle is a git URL that becomes its own project
        if (tokens.size() == 4 && property_name == WebDashConst::Public::kModuleFieldBundle) {
            WebDashType::GitProjectMetadata bundle_entry;
            bundle_entry.source = value;
            bundle_projects.push_back(bundle_entry);
            continue;
        }

        // Handle actions array (4 tokens: dependencies.0.actions.0)
        if (tokens.size() == 4 && property_name == WebDashConst::Public::kModuleFieldActions) {
            projects[array_index].actions.push_back(value);
            continue;
        }

        // Simple fields require exactly 3 tokens
        if (tokens.size() != 3) {
            continue;
        }

        // Field names
        // Handle singular "action" field (added to actions vector)
        if (property_name == WebDashConst::Public::kModuleFieldAction) {
            projects[array_index].actions.push_back(value);
        }
        if (property_name == WebDashConst::Public::kModuleFieldGit) {
            projects[array_index].source = value;
        }
        if (property_name == WebDashConst::Public::kModuleFieldPath) {
            projects[array_index].destination = value;
        }
        if (property_name == WebDashConst::Public::kModuleFieldRun) {
            projects[array_index].webdash_task = value;
        }
        if (property_name == WebDashConst::Public::kModuleFieldBranch) {
            projects[array_index].branch = value;
        }
        if (property_name == WebDashConst::Public::kModuleFieldAutoRegister) {
            projects[array_index].do_register = (value == WebDashConst::Internal::kJsonBoolTrue);
        }
        if (property_name == WebDashConst::Public::kModuleFieldWdir) {
            projects[array_index].wdir = value;
        }
    }

    vector<WebDashType::GitProjectMetadata> ret;

    // Add bundle projects first
    for (const auto& bundle_project : bundle_projects) {
        ret.push_back(bundle_project);
    }

    // Summarize all projects into a vector, each element representing one GIT project.
    for (const auto& element : projects) {
        auto project = element.second;
        const string path_substitution = "$.path()";

        // Resolve $.path() in wdir field to the project's destination path
        if (!project.wdir.empty()) {
            size_t pos = project.wdir.find(path_substitution);
            if (pos != string::npos) {
                project.wdir.replace(pos, path_substitution.length(), project.destination);
            }
        }

        // Resolve $.path() in actions to the project's destination path
        for (auto& action : project.actions) {
            size_t pos = action.find(path_substitution);
            while (pos != string::npos) {
                action.replace(pos, path_substitution.length(), project.destination);
                pos = action.find(path_substitution, pos + project.destination.length());
            }
        }

        ret.push_back(project);
    }

    return ret;
}


bool WebDashCore::IsInteractiveCloneEnabled() {
    string settings_prefix = string(WebDashConst::Public::kProfileKeySettings) + "." +
                             WebDashConst::Public::kSettingsInteractiveClone;

    auto defs = GetKeyValuesFromRootProfile();

    for (const auto& json_entry : defs) {
        if (json_entry.GetRootKey() != WebDashConst::Public::kProfileKeySettings) {
            continue;
        }

        const vector<string> tokens = json_entry.GetTokens();
        if (tokens.size() >= 2 && tokens[1] == WebDashConst::Public::kSettingsInteractiveClone) {
            return json_entry.GetValue() == WebDashConst::Internal::kJsonBoolTrue;
        }
    }

    // Default: no interactive prompts
    return false;
}


filesystem::path WebDashCore::_GetAndCreateLogDirectory() {
    const auto& webdash_root_path = GetWebDashRootDirectory();

    filesystem::path log_filepath = webdash_root_path;
    log_filepath /= WebDashConst::Public::kTemporaryStorageDir;
    log_filepath /= WebDashConst::Public::kLoggingSubdir;
    log_filepath /= _WEBDASH_PROJECT_NAME_;

    std::filesystem::create_directories(log_filepath);

    return log_filepath;
}


WebDashCore& WebDash() {
    return WebDashCore::Get();
}


std::optional<WebDashCore> WebDashCore::_singleton_instance = nullopt;

bool WebDashCore::_instance_creation_is_ongoing = false;
