#include "Suite.hpp"
#include <webdash-config.hpp>
#include <webdash-config-task.hpp>
#include <webdash-types.hpp>

#include <iostream>
#include <unistd.h>
#include <fstream>
#include <pwd.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <filesystem>
using namespace std;

namespace fs = std::filesystem;

static std::optional<fs::path> try_weakly_canonical(const fs::path& p) {
    try {
        return fs::weakly_canonical(p);
    } catch (...) {
        return std::nullopt;
    }
}

static std::string config_dir_relative_to_webdash_root(const std::string& config_path_str) {
    fs::path config_path{config_path_str};
    fs::path config_dir = config_path.parent_path();

    if (config_dir.empty()) {
        return "";
    }

    if (!config_path.is_absolute()) {
        return config_dir.generic_string();
    }

    const fs::path root_dir = WebDashCore::Get().GetWebDashRootDirectory();

    const auto root_canon = try_weakly_canonical(root_dir);
    const auto dir_canon = try_weakly_canonical(config_dir);
    if (!root_canon || !dir_canon) {
        try {
            return fs::relative(config_dir, root_dir).generic_string();
        } catch (...) {
            return config_dir.generic_string();
        }
    }

    try {
        fs::path rel = fs::relative(*dir_canon, *root_canon);
        // If outside root, don't return "../.." style paths.
        if (!rel.empty() && *rel.begin() == "..") {
            return config_dir.generic_string();
        }
        return rel.generic_string();
    } catch (...) {
        return config_dir.generic_string();
    }
}

static std::optional<fs::file_time_type> try_last_write_time(const std::string& path) {
    try {
        return fs::last_write_time(path);
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<std::string> try_read_project_name_from_config(const std::string& config_path) {
    try {
        std::ifstream in(config_path);
        if (!in.is_open()) return std::nullopt;
        // Accept JSONC ("//"/"/* */" comments, trailing commas), same as the
        // runtime config parser.
        json j = json::parse(in, nullptr, true, true, true);
        if (!j.is_object()) return std::nullopt;
        if (!j.contains("project-name")) return std::nullopt;
        const auto& v = j["project-name"];
        if (!v.is_string()) return std::nullopt;
        auto s = v.get<std::string>();
        if (s.empty()) return std::nullopt;
        return s;
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<int> try_read_priority_from_config(const std::string& config_path) {
    try {
        std::ifstream in(config_path);
        if (!in.is_open()) return std::nullopt;
        // Accept JSONC ("//"/"/* */" comments, trailing commas), same as the
        // runtime config parser.
        json j = json::parse(in, nullptr, true, true, true);
        if (!j.is_object()) return std::nullopt;
        if (!j.contains("priority")) return std::nullopt;
        const auto& v = j["priority"];
        if (v.is_number_integer()) return v.get<int>();
        if (v.is_number_float()) return static_cast<int>(v.get<double>());
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

Suite::Suite() {
    _is_alive = true;
}

Suite::~Suite() {
    _is_alive = false;

    _periodic_parse.join();
}

void Suite::LoadSaved() {
    WebDashCore::Get().LoadFromAppStorage("registered-configs.txt", WebDashType::StorageReadType::JSON, [&](istream& reader) {
        reader >> _json_source_data;

        json scripts = _json_source_data["scripts"];
        if (scripts.is_null())
            return;

        for (auto it : scripts) {
            // Support both old format (just path string) and new format (object with path and user info)
            if (it.is_string()) {
                const string path = it.get<std::string>();
                ifstream configStream;
                try {
                    WebDashCore::Get().Log(WebDashType::LogType::INFO, "Loading saved suite configuration: " + path);
                    configStream.open(path.c_str(), ifstream::in);
                } catch (...) {
                    WebDashCore::Get().Log(WebDashType::LogType::ERR, "Issues opening to config file (" + path + "). Something wrong with path?");
                    continue;
                }
                _scripts.emplace_back(WebDashConfig(path));
                _scripts.back().last_write_time = try_last_write_time(path);
                _scripts.back().project_name = try_read_project_name_from_config(path);
                _scripts.back().priority = try_read_priority_from_config(path);
            } else if (it.is_object()) {
                const string path = it["path"].get<std::string>();
                ifstream configStream;
                try {
                    WebDashCore::Get().Log(WebDashType::LogType::INFO, "Loading saved suite configuration: " + path);
                    configStream.open(path.c_str(), ifstream::in);
                } catch (...) {
                    WebDashCore::Get().Log(WebDashType::LogType::ERR, "Issues opening to config file (" + path + "). Something wrong with path?");
                    continue;
                }

                WebDashConfig cfg{path};
                RegisteredConfig rc{cfg};
                rc.last_write_time = try_last_write_time(path);
                rc.project_name = try_read_project_name_from_config(path);
                rc.priority = try_read_priority_from_config(path);
                if (it.contains("uid")) {
                    rc.run_as_uid = it["uid"].get<uid_t>();
                }
                if (it.contains("gid")) {
                    rc.run_as_gid = it["gid"].get<gid_t>();
                }
                if (it.contains("home")) {
                    rc.run_as_home = it["home"].get<std::string>();
                }
                _scripts.push_back(rc);
            }
        }
    });
}

void Suite::RegisterConfig(const WebDashConfig& config) {
    //
    // Check for the possibility that we were already given the config file.
    // If so, return (skip insertion).
    //

    for (const auto& it : _scripts)
        if (config.GetPath() == it.config.GetPath())
            return;

    {
        std::unique_lock<std::shared_mutex> lock(_suite_mutex);
        _scripts.emplace_back(config);
        _scripts.back().last_write_time = try_last_write_time(config.GetPath());
        _scripts.back().project_name = try_read_project_name_from_config(config.GetPath());
        _scripts.back().priority = try_read_priority_from_config(config.GetPath());
        SerializeAndSave();
    }

    NotifyChange();
}

void Suite::RegisterConfig(const WebDashConfig& config, uid_t uid, gid_t gid, const std::string& home) {
    //
    // Check for the possibility that we were already given the config file.
    // If so, update the user info.
    //

    for (auto& it : _scripts) {
        if (config.GetPath() == it.config.GetPath()) {
            {
                std::unique_lock<std::shared_mutex> lock(_suite_mutex);
                it.run_as_uid = uid;
                it.run_as_gid = gid;
                it.run_as_home = home;
                SerializeAndSave();
            }
            NotifyChange();
            return;
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(_suite_mutex);
        _scripts.emplace_back(config, uid, gid, home);
        _scripts.back().last_write_time = try_last_write_time(config.GetPath());
        _scripts.back().project_name = try_read_project_name_from_config(config.GetPath());
        _scripts.back().priority = try_read_priority_from_config(config.GetPath());
        SerializeAndSave();
    }

    NotifyChange();
}

void Suite::UnRegisterConfig(const string& path) {
    //
    // Remove config from the list if found.
    //

    bool changed = false;
    {
        std::unique_lock<std::shared_mutex> lock(_suite_mutex);
        for (auto it = _scripts.begin(); it != _scripts.end(); ++it) {
            try {
                if (path == it->config.GetPath()) {
                    _scripts.erase(it);
                    changed = true;
                    try {
                        SerializeAndSave();
                    } catch (...) {
                        // Ignore serialization errors during unregister
                    }
                    break;
                }
            } catch (...) {
                // Ignore exceptions during path comparison
            }
        }
    }

    if (changed) NotifyChange();
}
void Suite::ReloadAll() {
    {
        std::unique_lock<std::shared_mutex> lock(_suite_mutex);
        for (auto& it : _scripts) {
            it.config.Reload();
            it.last_write_time = try_last_write_time(it.config.GetPath());
            it.project_name = try_read_project_name_from_config(it.config.GetPath());
            it.priority = try_read_priority_from_config(it.config.GetPath());
        }
    }

    NotifyChange();
}

vector<string> Suite::GetRegisteredConfigPaths() {
    std::shared_lock<std::shared_mutex> lock(_suite_mutex);
    vector<string> paths;
    for (const auto& entry : _scripts)
        paths.push_back(entry.config.GetPath());
    return paths;
}

void Suite::Run() {
    _periodic_parse = std::thread([&]{
        // Time when periodic running started.
        const auto start = std::chrono::high_resolution_clock::now();

        // Time when last run happened.
        auto timeoflastrun = std::chrono::high_resolution_clock::now();

        while (_is_alive) {
            const auto curr = std::chrono::high_resolution_clock::now();
            const auto elapsed = duration_cast<std::chrono::milliseconds>(curr - timeoflastrun).count();
            [[maybe_unused]] const auto tot_elapsed = duration_cast<std::chrono::milliseconds>(curr - start).count();

            {
                const int log_every_sec = 100;
                if (elapsed > log_every_sec * 1000 /* 10 seconds */) {
                    timeoflastrun = std::chrono::high_resolution_clock::now();
                    // WebDashCore::Get().Log(WebDashType::LogType::INFO, "Another " + to_string(log_every_sec) + "; total seconds passed: " + to_string(tot_elapsed));
                }
            }

            // Delay for 1 second. There is no need to go any faster, yet.
            usleep(1000*1000);

            RunOnce();
        }
    });
}

void Suite::RunOnce() {
    // Step 1: quick auto-reload pass under exclusive lock.
    // This keeps config structures coherent for concurrent readers.
    {
        std::unique_lock<std::shared_mutex> lock(_suite_mutex);
        for (RegisteredConfig& entry : _scripts) {
            if (auto current_mtime = try_last_write_time(entry.config.GetPath())) {
                if (!entry.last_write_time.has_value() || *current_mtime != *entry.last_write_time) {
                    entry.config.Reload();
                    entry.last_write_time = current_mtime;
                    entry.project_name = try_read_project_name_from_config(entry.config.GetPath());
                    entry.priority = try_read_priority_from_config(entry.config.GetPath());
                }
            }
        }
    }

    // Step 2: execute scheduled tasks under a shared lock so portal/static-file reads don't hang
    // while long-running tasks are executing.
    std::shared_lock<std::shared_mutex> lock(_suite_mutex);
    for (RegisteredConfig& entry : _scripts) {
        WebDashType::RunConfig run_config;
        run_config.run_only_with_frequency = true;

        run_config.run_as_uid = entry.run_as_uid;
        run_config.run_as_gid = entry.run_as_gid;
        run_config.run_as_home = entry.run_as_home;

        auto results = entry.config.Run("" /* run all */, run_config);
        for (const auto& result : results) {
            if (!result.skipped) {
                RecordAction(entry.config.GetPath(), result.task_name, result.return_code);
            }
        }
    }
}

void Suite::SerializeAndSave() {
    WebDashCore::Get().WriteToAppStorage("registered-configs.txt", [&](WebDashType::StoreWriteChannel writer) {
        json config_arr;
        for (const auto& entry : _scripts) {
            json item;
            item["path"] = entry.config.GetPath();
            if (entry.run_as_uid.has_value()) {
                item["uid"] = entry.run_as_uid.value();
            }
            if (entry.run_as_gid.has_value()) {
                item["gid"] = entry.run_as_gid.value();
            }
            if (entry.run_as_home.has_value()) {
                item["home"] = entry.run_as_home.value();
            }
            config_arr["scripts"].push_back(item);
        }

        writer(WebDashType::StorageWriteType::Clear, "");
        writer(WebDashType::StorageWriteType::Append, config_arr.dump());
        writer(WebDashType::StorageWriteType::End, "");
    });
}

string Suite::GetRegisteredConfigsJsonDump() {
    std::shared_lock<std::shared_mutex> lock(_suite_mutex);
    json arr = json::array();

    for (const auto& entry : _scripts) {
        json item;
        item["path"] = entry.config.GetPath();
        item["rel"] = config_dir_relative_to_webdash_root(entry.config.GetPath());
        if (entry.run_as_uid.has_value()) {
            item["uid"] = entry.run_as_uid.value();
            item["gid"] = entry.run_as_gid.value_or(entry.run_as_uid.value());
            item["home"] = entry.run_as_home.value_or("");
        }
        if (entry.project_name.has_value()) {
            item["projectName"] = entry.project_name.value();
        }
        if (entry.priority.has_value()) {
            item["priority"] = entry.priority.value();
        }
        arr.push_back(item);
    }

    return arr.dump();
}

std::optional<RegisteredConfig> Suite::GetRegisteredConfig(const string& path) {
    std::shared_lock<std::shared_mutex> lock(_suite_mutex);

    for (const auto& entry : _scripts) {
        if (entry.config.GetPath() == path) {
            return entry;
        }
    }
    return std::nullopt;
}

void Suite::RecordAction(const std::string& config_path, const std::string& task_name, int return_code) {
    // Build ISO-8601 timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
    localtime_r(&time_t_now, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();

    ActionLogEntry entry;
    entry.timestamp = oss.str();
    entry.config_path = config_path;
    entry.task_name = task_name;
    entry.return_code = return_code;

    {
        std::lock_guard<std::mutex> lock(_action_log_mutex);
        _action_log.push_back(std::move(entry));
        while (_action_log.size() > kMaxActionLogSize) {
            _action_log.pop_front();
        }
    }

    // Notify listeners (e.g. WebSocket broadcast to portal)
    // Must be outside the lock — callback calls GetRecentActions() which also locks.
    NotifyChange();
}

vector<ActionLogEntry> Suite::GetRecentActions(size_t max_count) {
    std::lock_guard<std::mutex> lock(_action_log_mutex);

    vector<ActionLogEntry> result;
    size_t count = std::min(max_count, _action_log.size());
    // Return newest first
    for (size_t i = 0; i < count; ++i) {
        result.push_back(_action_log[_action_log.size() - 1 - i]);
    }
    return result;
}

bool Suite::RunTask(const string& config_path, const string& task_name) {
    // Copy what we need under the lock, then run outside it.
    std::optional<RegisteredConfig> found_entry;
    {
        std::shared_lock<std::shared_mutex> lock(_suite_mutex);
        for (RegisteredConfig& entry : _scripts) {
            if (entry.config.GetPath() != config_path)
                continue;
            found_entry = entry;
            break;
        }
    }
    if (!found_entry) return false;

    // Run in a detached thread so the API returns immediately.
    std::thread([this, entry = std::move(*found_entry), task_name]() mutable {
        WebDashType::RunConfig run_config;
        run_config.run_as_uid  = entry.run_as_uid;
        run_config.run_as_gid  = entry.run_as_gid;
        run_config.run_as_home = entry.run_as_home;
        run_config.skip_frequency_check = true;

        auto results = entry.config.Run(task_name, run_config);
        for (const auto& r : results) {
            if (!r.skipped) {
                RecordAction(entry.config.GetPath(), r.task_name, r.return_code);
            }
        }
    }).detach();

    return true;
}

vector<pair<string, string>> Suite::GetNetworkSharedPaths() {
    std::shared_lock<std::shared_mutex> lock(_suite_mutex);
    vector<pair<string, string>> result;

    for (const auto& entry : _scripts) {
        if (entry.config.IsNetworkShared()) {
            string project_name = entry.config.GetNetworkSharedName();
            auto dir = entry.config.GetNetworkSharedSourceDir();

            // Check for name conflicts
            for (const auto& [existing_name, existing_dir] : result) {
                if (existing_name == project_name) {
                    WebDash().Log(WebDashType::LogType::WARN,
                        "Static file server: name conflict for '" + project_name +
                        "' between '" + existing_dir + "' and '" + dir.string() +
                        "'. Use a string alias in \"network-shared\" to resolve.");
                }
            }

            result.emplace_back(project_name, dir.string());
        }
    }

    return result;
}
