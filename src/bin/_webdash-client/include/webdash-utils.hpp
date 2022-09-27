/**
 *  @note The term "WebDashJson key" has a unique meaning and should not be
 *        thought of relating to a JSON concept. It's merely computed using a
 *        chain of actual JSON keys.
**/

#pragma once

#include <webdash-exceptions.hpp>

#include <string>
#include <vector>
#include <filesystem>
#include <optional>
using namespace std;

using SubstitutionPair = pair<string,string>;


namespace {

    const string kEmptyStr = "";

} // namespace


namespace WebDashUtils {

    const string kWebDashJsonKeyPrefixID = "$#";


    /**
     * @brief epeatedly substitutes given key-value pairs until no further
     *        substitution is possible.
     * @param source The string on which to apply the substitutions.
     *               substitutions - List of {substring, replacement} entries.
     * @returns The resulting string after substitutions were performed.
     **/
    string ApplySubstitutions(
        const string& source,
        const vector<SubstitutionPair>& substitutions);


    /**
     *  @class Describes a JSON key-chain to its corresponding value.
    **/
    class JsonEntry {
        public:

            JsonEntry(const vector<string>& tokens, const string& value) {
                _key_path = tokens;
                _value = value;
            }

            const vector<string>& GetTokens() const {
                return _key_path;
            }

            const string& GetValue() const {
                return _value;
            }

            const string& GetRootKey() const {
                if (_key_path.size() > 0)
                    return _key_path.at(0);
                else
                    return kEmptyStr;
            }

            string GetSuffixKeyPath(size_t from_index) const {
                string ret = "";

                for (size_t index = from_index; index < _key_path.size(); ++index) {
                    ret += _key_path.at(index);
                    if (index + 1 < _key_path.size())
                        ret += ".";
                }

                return ret;
            }

            void ApplySubstitutionsInValue(const vector<SubstitutionPair>& replacements) {
                _value = ApplySubstitutions(_value, replacements);
            }

        private:

            vector<string> _key_path;

            string _value;
    };


    /**
     * @brief Concatenates all the JSON keys with the prefix
     *        kWebDashJsonKeyPrefixID into
     *
     *            {kWebDashJsonKeyPrefixID}.key1.key2.key3.....keyN
     *
     * @param json_entry A JSON entry.
     * @returns Concatenation of key values with prefix
     *           kWebDashJsonKeyPrefixID.
     **/
    string GetWebDashJsonKey(const JsonEntry& json_entry);


    /**
     * @brief Returns a key-value pair representing a JSON entry for special
     *        purposes. The key is computed as per by GetWebDashJsonKey(),
     *        while the value is simply the value that the JSON key-chain
     *        points to.
     * @param json_entry A JSON entry.
     * @returns Key-value pair with the specially-formatted chain of JSON keys
     *          and the value they point to.
     **/
    pair<string, string> GenerateWebDashConfigSubstitution(const JsonEntry& json_entry);


    /**
     * @brief Parses the given path as a JSON file and returns the
     *        key(-chain)-value pairs using the JsonEntry type.
     *
     *        Complexity:
     *              Not an efficient implementation!
     *
     *              Worst-case: is O(|num of JSON values| * |sum of all key all
     *                          value string lengths|).
     *
     * @param filepath Path to the JSON file.
     *
     * @returns List of key(-chain)-value pairs as a list of JsonEntry.
     **/
    vector<JsonEntry> ParseJSON(const filesystem::path& filepath);


    /**
     * @brief Given a string, will find the last '/' character and return the
     *        preix up to that character. If no such character is found, an
     *        empty string ("") is returned.
     *
     * @param full_filename Full file path of the file for which to determine
     *                      the directory.
     * @returns The returned prefix of a given full file path
     *          (e.g., mnt/c/dir/hello.txt), denoting the directory
     *          (e.g., /mnt/c/dir/).
     **/
    string GetDirectoryOfFilepath(const string& filepath);
}
