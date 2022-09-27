/**
 * Customized exception handling. Has some rudimentary stack printing ability
 * for UNIX environments.
 *
 * FYI: In C++-23, a non-platform dependent exception handling is added.
 **/

#pragma once

#include <iostream>
#include <stdexcept>
#include <string>
#include <memory>
#include <vector>
#include <filesystem>

#ifdef _PLATFORM_LINUX
    #define BACKTRACE_BUFFER_SIZE 100
    #include <execinfo.h>
    #include <errno.h>
    #include <string.h>
#endif // _PLATFORM_LINUX


namespace WebDashException {

    class General : public std::runtime_error {
        public:
            void ComputeBacktrace() {
#ifdef _PLATFORM_LINUX
                std::unique_ptr<void*[]> backtrace_buffer(new void*[BACKTRACE_BUFFER_SIZE]);


                /**
                 * @note These addresses point to the corresponding stack
                 *       frames. @warning Do not access this buffer outside of
                 *       this scope; unique_ptr is used for important reasons.
                 */

                int backtrace_frames_count = backtrace(
                    backtrace_buffer.get(),
                    BACKTRACE_BUFFER_SIZE);

                char **strings = backtrace_symbols(
                    backtrace_buffer.get(),
                    backtrace_frames_count);

                if (strings == NULL) {
                    throw std::runtime_error(std::string("Error calling backtrace_symbols(): ") + strerror(errno));
                }

                for (int i = 0; i < backtrace_frames_count; ++i) {
                    backtrace_symbol_names.push_back(strings[i]);
                }
#endif // _PLATFORM_LINUX
            }

            General(const std::string msg) : runtime_error(msg) {
                ComputeBacktrace();
            }

            void PrintBacktrace(std::ostream& out) {
                if (backtrace_symbol_names.size() == 0) {
                    out << "Backtrace not available" << std::endl;
                    return;
                }

                size_t index = 0;
                out << "Backtrace:" << std::endl;
                for (const auto& entry : backtrace_symbol_names) {
                    out << "[" << std::to_string(index++) + "]: "
                        << entry << std::endl;
                }

                out.flush();
            }

            ~General() {

            }

        private:

            std::vector<std::string> backtrace_symbol_names;
    };

    class FileNotFound : public General {
        public:
            FileNotFound(const std::string msg) : General(msg) {};
    };

    class NoWebDashRootFound : public General {
        public:
            NoWebDashRootFound(const std::string msg,
                        const std::filesystem::path last_parsed_json_filepath ,
                        const std::string last_json_parse_error) : General(
                            msg + (last_json_parse_error != "" ?
                                    " (last attempted JSON '" + last_parsed_json_filepath.string() +
                                    "' failed with message:" + last_json_parse_error : "")
                            )
            {}
    };

    class ConfigJsonParseError : public General {
        public:
            ConfigJsonParseError(const std::string msg) : General(msg) {};
    };
}