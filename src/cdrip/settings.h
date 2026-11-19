#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include "cdrip/cdrip.h"

namespace cdrip::detail {

/** Normalize an enumerated setting without changing path or tag values. */
inline std::string normalized_setting(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

/** Parse a complete signed decimal integer, rejecting overflow and suffixes. */
inline bool parse_setting_integer(const std::string& value, int& result) {
    if (value.empty()) return false;
    size_t begin = (value[0] == '+' || value[0] == '-') ? 1 : 0;
    if (begin == value.size() || value.find_first_not_of("0123456789", begin) != std::string::npos) return false;
    try { result = std::stoi(value); return true; }
    catch (...) { return false; }
}

/** Parse the supported boolean spellings used by configuration files. */
inline bool parse_setting_boolean(const std::string& raw, bool& result) {
    const auto value = normalized_setting(raw);
    if (value == "true" || value == "1") { result = true; return true; }
    if (value == "false" || value == "0") { result = false; return true; }
    return false;
}

/** Parse auto or a FLAC compression level from zero through eight. */
inline bool parse_setting_compression(const std::string& raw, int& result) {
    const auto value = normalized_setting(raw);
    if (value == "auto") { result = -1; return true; }
    return parse_setting_integer(value, result) && result >= 0 && result <= 8;
}

/** Parse a drive reading mode independently of AccurateRip verification. */
inline bool parse_setting_mode(const std::string& raw, CdRipRipModes& result) {
    const auto value = normalized_setting(raw);
    if (value == "best") result = RIP_MODES_BEST;
    else if (value == "fast") result = RIP_MODES_FAST;
    else if (value == "default") result = RIP_MODES_DEFAULT;
    else return false;
    return true;
}

/** Test a normalized terminal preview mode. */
inline bool valid_cover_art_mode(const std::string& value) {
    return value == "auto" || value == "kitty" || value == "ascii" || value == "none";
}

/** Test a normalized cover art source preference. */
inline bool valid_discogs_mode(const std::string& value) {
    return value == "no" || value == "always" || value == "fallback";
}
}
