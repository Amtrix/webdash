#include "webdash-utils.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <queue>

using namespace std;
using json = nlohmann::json;


namespace {

    constexpr char kJsonBooleanTrueAsString[] = "true";
    constexpr char kJsonBooleanFalseAsString[] = "false";

    /**
     * @brief Attempts to perform a key-value substitution on a given string.
     * @param text The string in which to look for a substring to substitute.
     * @returns True if-and-only-if a substitution was found and performed.
     */
    bool PerformSubstitution(string& text, const SubstitutionPair& substitution) {
        size_t keyword_position = text.find(substitution.first);

        if (keyword_position == string::npos)
            return false;

        text.replace(keyword_position, substitution.first.size(), substitution.second);

        return true;
    }


    /**
     * @brief Given a JSON value object, returns its string representation.
     * @param The json element.
     * @returns The string representation of the given JSON element. Throws an exception if the conversion is not
     *          possible.
     */
    string BasicJsonValueToString(const json& element) {
        if (element.is_string()) {
            return element.get<std::string>();
        }

        if (element.is_boolean()) {
            return element.get<bool>() ? kJsonBooleanTrueAsString : kJsonBooleanFalseAsString;
        }

        if (element.is_number_integer()) {
            return to_string(element.get<int>());
        }

        if (element.is_number()) {
            return to_string(element.get<double>());
        }

        throw WebDashException::General("The given JSON element cannot be represented as a string value.");
    }

} // namespace


namespace WebDashUtils {

    string GetWebDashJsonKey(const JsonEntry& json_entry) {
        string key_identifier = kWebDashJsonKeyPrefixID;

        for (auto& token : json_entry.GetTokens()) {
            key_identifier += "." + token;
        }

        return key_identifier;
    }


    pair<string, string> GenerateWebDashConfigSubstitution(const JsonEntry& json_entry) {
        return { GetWebDashJsonKey(json_entry), json_entry.GetValue() };
    }


    string GetDirectoryOfFilepath(const string& filepath) {
        const size_t last_slash_pos = filepath.find_last_of("\\/");

        if (last_slash_pos == std::string::npos) {
            return "";
        }

        return filepath.substr(0, last_slash_pos);
    }


    vector<JsonEntry> ParseJSON(const filesystem::path& filepath) {
        vector<JsonEntry> keychain_to_values_in_profile;

        ifstream configStream;
        configStream.open(filepath.string().c_str(), ifstream::in);

        // N.B. Parsing can throw exceptions.
        json profile_content;
        configStream >> profile_content;

        // Determined a chain of key identifiers and the values they point to in JSON. Add to return.
        auto AddElementToReturn = [&](vector<string> chain, const auto& element) -> void {
            auto element_as_string = BasicJsonValueToString(element);
            keychain_to_values_in_profile.push_back({std::move(chain), std::move(element_as_string)});
        };

        // Objects and arrays can only be further recursed into.
        auto IsFinalValue = [](const json& element) -> bool {
            return !(element.is_object() || element.is_array());
        };

        /**
         * Parse the JSON file using BFS.
         */

        queue<pair<vector<string>, json>> Q;
        Q.push({{}, profile_content});

        while (!Q.empty()) {
            const auto u = Q.front();
            Q.pop();

            /**
             * If the currently parsed key has an array as value, parse the values while appending the array index
             * as "[index]" to the chain.
             */
            if (u.second.is_array()) {
                size_t array_index = -1;

                for (const auto& array_value_element : u.second) {
                    array_index++;
                    const string array_entry_key = "[" + to_string(array_index) + "]";

                    vector<string> chain = u.first;
                    chain.push_back(array_entry_key);

                    if (!IsFinalValue(array_value_element)) {
                        Q.push(make_pair(std::move(chain), array_value_element));
                    } else {
                        AddElementToReturn(std::move(chain), array_value_element);
                    }
                }
            } else if (u.second.is_object()) {

                for (const auto& [key, value_element] : u.second.items()) {
                    vector<string> chain = u.first;
                    chain.push_back(key);

                    if (!IsFinalValue(value_element)) {
                        Q.push(make_pair(std::move(chain), value_element));
                    } else {
                        AddElementToReturn(std::move(chain), value_element);
                    }
                }
            } else {
                throw WebDashException::General("Tried to expand non-object/non-array JSON element.");
            }
        }

        return keychain_to_values_in_profile;
    }


    string ApplySubstitutions(
            const string& source,
            const vector<SubstitutionPair>& substitutions) {

        string ret = source;

        bool substitution_possible = true;

        while (substitution_possible) {
            substitution_possible = false;

            for (const auto& sub : substitutions) {
                substitution_possible |= PerformSubstitution(ret, sub);
            }
        }

        return ret;
    }

} // namespace WebDashUtils
