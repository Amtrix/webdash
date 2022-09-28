#pragma once

// Self
#include <webdash-config.hpp>
#include <webdash-core.hpp>

// Standard
#include <cstdio>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <algorithm>

// External
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace std;
using json = nlohmann::json;


/**
 * @brief Extends the given `text` string by repeatedly adding a given character `padding` until the
 *        length of the string equals to the value of `final_length`.
 * @param final_length The total length of the resulting string.
 * @param string equals to the given `final_length` value.
 * @throws std::logic_error Thrown if the initial length of the string exceeds `final_length`.
 * @returns The padded string.
 */
string AddLeftPadding(const size_t final_length,
                      const char padding,
                      const string& text) {

    if (final_length < text.size())
        throw std::logic_error("Text length bigger than the final request length.");

    string ret = string(final_length - text.length(), padding) + text;
    return ret;
}
