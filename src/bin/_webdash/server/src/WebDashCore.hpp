#pragma once

#include <webdash-config.hpp>


namespace Core {
    void RegisterConfig(WebDashConfig);

    vector<string> GetConfigPaths();
}