#include "Suite.hpp"
#include <webdash-config.hpp>
#include <webdash-types.hpp>

#include <iostream>
#include <unistd.h>
#include <fstream>
using namespace std;

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
            const string path = it.get<std::string>();

            ifstream configStream;
            try {
                WebDashCore::Get().Log(WebDashType::LogType::INFO, "Loading saved suite configuration: " + path);
                configStream.open(path.c_str(), ifstream::in);
            } catch (...) {
                WebDashCore::Get().Log(WebDashType::LogType::ERR, "Issues opening to config file (" + path + "). Something wrong with path?");
                continue;
            }

            _scripts.emplace_back(path);
        }
    });
}

void Suite::RegisterConfig(const WebDashConfig& config) {
    //
    // Check for the possibility that we were already given the config file.
    // If so, return (skip insertion).
    //

    for (const auto& it : _scripts)
        if (config.GetPath() == it.GetPath())
            return;

    std::lock_guard<std::mutex> lock(_suite_mutex);

    _scripts.push_back(config);

    SerializeAndSave();
}

void Suite::UnRegisterConfig(const string& path) {
    //
    // Check for the possibility that we were already given the config file.
    // If so, return (skip insertion).
    //

    for (auto it = _scripts.begin(); it != _scripts.end(); ++it) {
        if (path == it->GetPath()) {
            std::lock_guard<std::mutex> lock(_suite_mutex);
            _scripts.erase(it);
            SerializeAndSave();
            break;
        }
    }
}

void Suite::ReloadAll() {
    for (auto& it : _scripts) {
        it.Reload();
    }
}

vector<string> Suite::GetRegisteredConfigPaths() {
    vector<string> paths;
    for (const auto& entry : _scripts)
        paths.push_back(entry.GetPath());
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
            const auto tot_elapsed = duration_cast<std::chrono::milliseconds>(curr - start).count();

            {
                const int log_every_sec = 100;
                if (elapsed > log_every_sec * 1000 /* 10 seconds */) {
                    timeoflastrun = std::chrono::high_resolution_clock::now();
                    WebDashCore::Get().Log(WebDashType::LogType::INFO, "Another " + to_string(log_every_sec) + "; total seconds passed: " + to_string(tot_elapsed));
                }
            }

            // Delay for 1 second. There is no need to go any faster, yet.
            usleep(1000*1000);

            RunOnce();
        }
    });
}

void Suite::RunOnce() {
    std::lock_guard<std::mutex> lock(_suite_mutex);

    WebDashType::RunConfig run_config;
    run_config.run_only_with_frequency = true;

    for (WebDashConfig& entry : _scripts)
        entry.Run("" /* run all */, run_config);
}

void Suite::SerializeAndSave() {
    WebDashCore::Get().WriteToAppStorage("registered-configs.txt", [&](WebDashType::StoreWriteChannel writer) {
        json config_arr;
        for (const auto& entry : _scripts)
            config_arr["scripts"].push_back(entry.GetPath());

        writer(WebDashType::StorageWriteType::Clear, "");
        writer(WebDashType::StorageWriteType::Append, config_arr.dump());
        writer(WebDashType::StorageWriteType::End, "");
    });
}

string Suite::GetRegisteredConfigsJsonDump() {
    json arr;

    for (const auto& entry : _scripts)
        arr.push_back(entry.GetPath());

    return arr.dump();
}
