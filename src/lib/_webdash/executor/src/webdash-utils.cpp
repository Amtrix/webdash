#include "webdash-utils.hpp"
#include "webdash-constants.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <sys/wait.h>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <fstream>
#include <queue>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

using namespace std;
using json = nlohmann::json;


namespace {

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
            return element.get<bool>() ? WebDashConst::Internal::kJsonBoolTrue : WebDashConst::Internal::kJsonBoolFalse;
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

    CommandResult ExecuteCommand(const std::string& command) {
        CommandResult result;
        result.exit_code = -1;

        // Redirect stderr to stdout so we capture both
        std::string full_command = command + " 2>&1";

        FILE* pipe = popen(full_command.c_str(), "r");
        if (!pipe) {
            result.output = "Failed to execute command: popen() failed";
            return result;
        }

        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result.output += buffer;
        }

        int status = pclose(pipe);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        return result;
    }

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
                size_t array_index = 0;

                for (const auto& array_value_element : u.second) {
                    // Build array key like "[0]", "[1]", etc.
                    // Using ostringstream avoids a spurious GCC 12 -Wrestrict warning
                    // that occurs with "[" + to_string(x) + "]" concatenation.
                    std::ostringstream oss;
                    oss << "[" << array_index << "]";
                    const string array_entry_key = oss.str();
                    array_index++;

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


    string ResolvePortReferences(
            const string& source,
            const vector<JsonEntry>& profile_entries) {

        string result = source;
        const string port_prefix = "$.port(";

        size_t pos = 0;
        while ((pos = result.find(port_prefix, pos)) != string::npos) {
            size_t end_pos = result.find(')', pos);
            if (end_pos == string::npos) {
                // No closing paren found, skip this occurrence
                pos += port_prefix.length();
                continue;
            }

            // Extract the identifier between $.port( and )
            string port_id = result.substr(pos + port_prefix.length(),
                                           end_pos - pos - port_prefix.length());

            // Look up in profile under "ports.<identifier>"
            string port_value;
            bool found = false;
            for (const auto& entry : profile_entries) {
                const auto& tokens = entry.GetTokens();
                if (tokens.size() == 2 && tokens[0] == "ports" && tokens[1] == port_id) {
                    port_value = entry.GetValue();
                    found = true;
                    break;
                }
            }

            if (found) {
                // Replace $.port(identifier) with the port value
                result.replace(pos, end_pos - pos + 1, port_value);
                // Don't advance pos, in case there are nested or adjacent patterns
            } else {
                // Not found, skip past this pattern
                pos = end_pos + 1;
            }
        }

        return result;
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


    bool IsPortUnused(int port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return false;  // Can't create socket, assume port is in use
        }

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // Connect to localhost
        addr.sin_port = htons(port);

        // Try to connect - if connection succeeds, port is IN USE
        int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        close(sock);

        // If connect failed, port is unused (nothing listening)
        return result != 0;
    }


    vector<string> TokenizeCommand(const string& command) {
        vector<string> tokens;
        string current_token;
        bool in_single_quote = false;
        bool in_double_quote = false;
        bool escape_next = false;

        for (size_t i = 0; i < command.size(); ++i) {
            char c = command[i];

            if (escape_next) {
                // Backslash escape: add the character literally
                current_token += c;
                escape_next = false;
                continue;
            }

            if (c == '\\' && !in_single_quote) {
                // Backslash outside single quotes starts an escape sequence
                // (single quotes treat backslash literally in shell)
                escape_next = true;
                continue;
            }

            if (c == '\'' && !in_double_quote) {
                // Toggle single quote mode (unless inside double quotes)
                in_single_quote = !in_single_quote;
                continue;
            }

            if (c == '"' && !in_single_quote) {
                // Toggle double quote mode (unless inside single quotes)
                in_double_quote = !in_double_quote;
                continue;
            }

            if (isspace(c) && !in_single_quote && !in_double_quote) {
                // Whitespace outside quotes: end current token
                if (!current_token.empty()) {
                    tokens.push_back(current_token);
                    current_token.clear();
                }
                continue;
            }

            // Regular character: add to current token
            current_token += c;
        }

        // Add final token if any
        if (!current_token.empty()) {
            tokens.push_back(current_token);
        }

        return tokens;
    }

} // namespace WebDashUtils
