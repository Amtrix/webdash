/**
 * (WebDash) Profile (JSON file) - This is a JSON file whose name equals to that
 * of the kRootProfileFilename's string content. WebDash has a root directory
 * and it is identified through that file. It stores various entries that
 * configure WebDash's global behavior. This includes the environment variables
 * that WebDash adds, the entries to the PATH variable, the GitHub projects to
 * manage, etc.
 */

#pragma once

#include <webdash-utils.hpp>
#include <webdash-types.hpp>

#include <string>
#include <optional>
#include <vector>
#include <functional>
#include <filesystem>
#include <map>

using namespace std;


// Must be specified by consuming libraries. Used to identify the individual projects.
// Used for logging, determining the APP's storage location, etc.
extern const string _WEBDASH_PROJECT_NAME_;


class WebDashCore {
    private:

        static constexpr char kMagicWebDashKeyInProfile[] = "$#.myworld.rootDir";
        static constexpr char kMagicWebDashValueInProfile[] = "this";

        // Must equal to kMagicWebDashKeyInProfile without the first prefix
        // until the first period (inclusive).
        static constexpr char kMagicKeyInProfileErrorMessage[] = "myworld.rootDir";

        static constexpr char kRootProfileFilename[] = "webdash-profile.json";

        /**
         * Only used to allow this class to offer a private "key" to other
         * classes that wish to use the default constructor.
         */
        struct PrivateCtorClass {};

    public:

        /**
         * This class is used as a singleton. The only way to get an instance is
         * through the ::Get() function.
         *
         * OBSERVE: The _singleton_instance member holds the singleton and we set it through the emplace() call. Emplace
         *          needs access to the matching default constructor, which we want to keep innaccessible to the outside.
         *          To go around this issue, we **do** create a publicly accessible default constructor but require a
         *          privately-typed argument. Given that only this class can provide that argument, only it can make use
         *          of that default constructor.
         */
        WebDashCore(PrivateCtorClass private_ctor);
        WebDashCore(const WebDashCore&) = delete;
        static WebDashCore& Get();


        /**
         *  @brief Returns keyword aliasing for any WebDash related config parsing, including substitutions for the
         *         WebDash Profile JSON (kRootProfileFilename) itself.
         *  @returnsList of substitutions.
         */
        vector<SubstitutionPair> GetPrimaryKeywordSubstitutions() const;


        /**
         *  @brief Returns a list of all key-chain-to-value pairs from WebDash's Profile JSON file.
         *  @returnsList of all JSON value entries from the Profile file.
         */
        const vector<WebDashUtils::JsonEntry>& GetKeyValuesFromRootProfile() const;


        /**
         *  @brief Returns the root directory, which holds WebDash's Profile file.
         *  @returnsThe WebDash root directory.
         */
        const filesystem::path& GetWebDashRootDirectory() const;


        /**
         *  @brief For the project that includes this library, it will return the project-specific directory for
         *         PERSISTENT file storage.
         *  @returnsProject's persistent file storage.
         */
        std::filesystem::path GetPersistenteAppStoragePath() const;


        /**
         *  @brief Through the given callback, the caller receives an argument of type StoreWriteChannel that it can use
         *         to perform the writes on the request file.
         *  @param filename The name of the file to which to write to.
         *  @param callback The callback to which to provide a writing channel to.
         */
        void WriteToAppStorage(
            const string filename,
            std::function<void(WebDashType::StoreWriteChannel)> callback);


        /**
         *  @brief Through the given callback, the caller receives an argument of type istream that it can use to read a
         *         file from its persistent storage location.
         *  @param filename The name of the file from which to read to.
         *  @param type The type of file that is being red. Certain files have customized reading options for ease of
         *              use. This also includes specialized default content when the file does not exist.
         *  @param allback The callback to which to provide the read channel.
         */
        void LoadFromAppStorage(const string& filename,
                                const WebDashType::StorageReadType& type,
                                std::function<void(istream&)> callback);


        /**
         *  @brief Adds log statements for the project that includes this WebDash library. The log files are stored in the
         *         temporary storage of the project (i.e., app-temporary/logging/<project name>).
         *  @param type The type of log statement (e.g., DEBUG, INFO, etc.).
         *  @param msg The log message.
         *  @param keep_version_from_previous_execution If the program was newly started, deletes the previous file (if
         *                                              it exists) iff set to true.
         */
        void Log(const WebDashType::LogType type,
                 const std::string msg,
                 const bool keep_version_from_previous_execution = false);

        /**
         *  @brief Special type of log message. Never deletes previous entry and uses LogType::NOTIFY.
         *  @param msg The log message.
         */
        void Notify(const std::string msg);


        /**
         *  @brief Returns a list of entries to add to PATH (as parsed by the JSON profile under the "path-add" key).
         *  @returnsList of entries to add to the PATH variable.
         */
        vector<string> GetEnvPathAdditions();


        /**
         *  @brief Returns a list of environment variables to add (as parsed by the JSON profile under the "env" key).
         *  @returnsList of environment variables to add.
         */
        vector<SubstitutionPair> GetEnvironmentAdditions();


        /**
         *  @brief Returns a list of Git projects that are stored in the WebDash profile.
         *  @returnsList containing metadata of GitHub projects, as given in the profile.
         */
        vector<WebDashType::GitProjectMetadata> GetExternalGitProjects();


    private:


        /**
         *  @brief Return path of logging directory. Creates the directory if it does not exist. Directory:
         *         _GetAndCreateLogDirectory()/app-temporary/logging/_WEBDASH_PROJECT_NAME_
         *  @returnsList containing metadata of GitHub projects, as given in the profile.
         */
        filesystem::path _GetAndCreateLogDirectory();


        /**
         *  @brief Determines the root of the webdash-managed directory structure, which is the directory with the JSON
         *         Profile file (kRootProfileFilename) file with the key-value pair:
         *
         *               kMagicKeyInProfileErrorMessage : kMagicWebDashValueInProfile
         *
         *         The search for this file happens through ancestry-traversal, starting with the current work
         *         directory (cwd).
         */
        void _CalculateRootDirectory();


        /**
         *  @brief Clears (and/or creates) the log files for: Error, Info, Warn, Debug.
         */
        void _InitializeLogFiles();


        /**
         *  @brief Adds log statements for the project that includes this WebDash library. The log files are stored in
         *         the temporary storage of the project (i.e., app-temporary/logging/<project name>).
         *  @param profile_filepath The path to determined the WebDash profile JSON file.
         *  @param key_values The parsed key-value pairs from the WebDash profile.
         */
        void _FinalizeInitialization(filesystem::path profile_filepath, vector<WebDashUtils::JsonEntry> key_values);


        // Determines if the given LogType's file was previously cleared during this process' execution.
        std::map<WebDashType::LogType, bool> _logfile_was_cleared;

        // Holds the WebDashCore singleton instance, once created.
        static std::optional<WebDashCore> _singleton_instance;

        // The WebDash root directory that contains the JSON Profile file.
        filesystem::path _webdash_root_directory;

        // The key-chain-to-value pairs, parsed from the WebDash Profile JSON file.
        vector<WebDashUtils::JsonEntry> _profile_key_values;

        // Boolean to prevent self-accessing the singleton during creation.
        static bool _instance_creation_is_ongoing;
};


/**
 *  @brief Shortcut to retrieve the WebDashCore singleton.
 *  @returnsThe singleton.
 */
WebDashCore& WebDash();


/**
 * @namespace Handy routines meant to provide shortcuts to WebDash() calls.
 *            These reduce the indirection of type
 *                      WebDashCore::Get().Log(...)
 *                          to
 *                      IWebDash::Notify(....)
 */
namespace IWebDash {

    /**
     * @brief Appends a notifying log line, without clearning any previous
     *        content, irrespective of if the process was
     * @param msg The message to log.
     */
    inline void Notify(const std::string msg) {
        WebDash().Log(WebDashType::LogType::NOTIFY, msg, true);
    }
}