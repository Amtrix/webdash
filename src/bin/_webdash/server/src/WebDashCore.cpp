
#include <webdash-core.hpp>
#include <webdash-config.hpp>

#include "WebDashCore.hpp"

namespace Core {
    void RegisterConfig(WebDashConfig config) {
        WebDash().WriteToAppStorage("registered-configs.txt", [&](WebDashType::StoreWriteChannel writer) {
            writer(WebDashType::StorageWriteType::Append, config.GetPath());
        });
    }

    vector<string> GetConfigPaths() {
        vector<string> paths;

        return paths;
    }
}
