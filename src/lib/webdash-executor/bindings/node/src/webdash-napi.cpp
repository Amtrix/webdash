/**
 * N-API bindings for webdash-executor library.
 * Exposes WebDashCore functionality to Node.js applications.
 */

#define NAPI_VERSION 8
#include <node_api.h>

#include <webdash-core.hpp>
#include <webdash-types.hpp>

#include <string>
#include <map>

// Define the project name - required by webdash-executor
// This is set at compile time; for runtime configuration, the project using
// these bindings should be built with the appropriate -D flag.
#ifndef WEBDASH_PROJECT_NAME
#define WEBDASH_PROJECT_NAME "webdash-node"
#endif

const std::string _WEBDASH_PROJECT_NAME_ = WEBDASH_PROJECT_NAME;

namespace {

/**
 * Helper to throw a JavaScript error.
 */
void ThrowError(napi_env env, const char* message) {
    napi_throw_error(env, nullptr, message);
}

/**
 * Helper to get a string from a napi_value.
 */
std::string GetString(napi_env env, napi_value value) {
    size_t length;
    napi_get_value_string_utf8(env, value, nullptr, 0, &length);
    std::string result(length, '\0');
    napi_get_value_string_utf8(env, value, &result[0], length + 1, &length);
    return result;
}

/**
 * Map string log types to WebDashType::LogType enum.
 */
WebDashType::LogType StringToLogType(const std::string& type) {
    static const std::map<std::string, WebDashType::LogType> mapping = {
        {"info",   WebDashType::LogType::INFO},
        {"error",  WebDashType::LogType::ERR},
        {"warn",   WebDashType::LogType::WARN},
        {"notify", WebDashType::LogType::NOTIFY},
        {"debug",  WebDashType::LogType::DEBUG}
    };

    auto it = mapping.find(type);
    if (it != mapping.end()) {
        return it->second;
    }
    return WebDashType::LogType::INFO;
}

} // namespace


/**
 * webdash.log(type: string, message: string)
 *
 * Log a message using WebDashCore's logging system.
 * Types: "info", "error", "warn", "notify", "debug"
 */
napi_value Log(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 2) {
        ThrowError(env, "log() requires 2 arguments: type and message");
        return nullptr;
    }

    std::string type = GetString(env, argv[0]);
    std::string message = GetString(env, argv[1]);

    try {
        WebDashCore::Get().Log(StringToLogType(type), message);
    } catch (const std::exception& e) {
        ThrowError(env, e.what());
        return nullptr;
    }

    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
}


/**
 * webdash.notify(message: string)
 *
 * Log a notification message (never rotated).
 */
napi_value Notify(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    if (argc < 1) {
        ThrowError(env, "notify() requires 1 argument: message");
        return nullptr;
    }

    std::string message = GetString(env, argv[0]);

    try {
        WebDashCore::Get().Notify(message);
    } catch (const std::exception& e) {
        ThrowError(env, e.what());
        return nullptr;
    }

    napi_value undefined;
    napi_get_undefined(env, &undefined);
    return undefined;
}


/**
 * webdash.getRootDirectory(): string
 *
 * Get the WebDash root directory path.
 */
napi_value GetRootDirectory(napi_env env, napi_callback_info info) {
    (void)info;

    try {
        std::string path = WebDashCore::Get().GetWebDashRootDirectory().string();

        napi_value result;
        napi_create_string_utf8(env, path.c_str(), path.length(), &result);
        return result;
    } catch (const std::exception& e) {
        ThrowError(env, e.what());
        return nullptr;
    }
}


/**
 * webdash.getProjectName(): string
 *
 * Get the current project name.
 */
napi_value GetProjectName(napi_env env, napi_callback_info info) {
    (void)info;

    napi_value result;
    napi_create_string_utf8(env, _WEBDASH_PROJECT_NAME_.c_str(), _WEBDASH_PROJECT_NAME_.length(), &result);
    return result;
}


/**
 * Module initialization.
 */
napi_value Init(napi_env env, napi_value exports) {
    // Define the functions to export
    napi_property_descriptor descriptors[] = {
        {"log", nullptr, Log, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"notify", nullptr, Notify, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getRootDirectory", nullptr, GetRootDirectory, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getProjectName", nullptr, GetProjectName, nullptr, nullptr, nullptr, napi_default, nullptr},
    };

    napi_define_properties(env, exports, sizeof(descriptors) / sizeof(descriptors[0]), descriptors);

    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
