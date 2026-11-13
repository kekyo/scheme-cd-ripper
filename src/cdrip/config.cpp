// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <glib.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "internal.h"
#include "settings.h"

using namespace cdrip::detail;

/* ------------------------------------------------------------------- */
/* Use static linkage for file local definitions */

static CdRipCddbServer make_cddb_server(
    const std::string& host,
    int port,
    const std::string& path,
    const std::string& label) {

    CdRipCddbServer s{};
    s.name = make_cstr_copy(host);
    s.port = port;
    s.path = make_cstr_copy(path);
    s.label = make_cstr_copy(label);
    return s;
}

static CdRipCddbServerList* make_builtin_servers() {
    auto* list = new CdRipCddbServerList{};
    list->count = 3;
    list->servers = new CdRipCddbServer[list->count]{
        make_cddb_server("", 80, "", "musicbrainz"),
        make_cddb_server("gnudb.gnudb.org", 80, "/~cddb/cddb.cgi", "gnudb"),
        make_cddb_server("freedb.dbpoweramp.com", 80, "/~cddb/cddb.cgi", "dbpoweramp"),
    };
    return list;
}

static void replace_cstr(
    const char*& target,
    const std::string& value) {

    release_cstr(target);
    target = make_cstr_copy(value);
}

static std::vector<std::string> split_list(
    const std::string& s) {

    std::vector<std::string> result;
    std::string current;
    for (char ch : s) {
        if (ch == ',' || ch == ';') {
            const std::string trimmed = trim(current);
            if (!trimmed.empty()) result.push_back(trimmed);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    const std::string trimmed = trim(current);
    if (!trimmed.empty()) result.push_back(trimmed);
    return result;
}

static std::string strip_inline_comment_value(
    const std::string& raw) {

    bool in_single = false;
    bool in_double = false;
    bool escaped = false;
    for (size_t i = 0; i < raw.size(); ++i) {
        const char ch = raw[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (ch == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }
        if (!in_single && !in_double && (ch == '#' || ch == ';')) {
            if (i == 0 || std::isspace(static_cast<unsigned char>(raw[i - 1]))) {
                return trim(raw.substr(0, i));
            }
        }
    }
    return trim(raw);
}

static void release_cddb_servers(
    std::vector<CdRipCddbServer>& servers) {

    for (auto& s : servers) {
        release_cstr(s.name);
        release_cstr(s.path);
        release_cstr(s.label);
    }
    servers.clear();
}

static CdRipConfig* make_default_config() {
    auto* cfg = new CdRipConfig{};
    cfg->device = nullptr;
    cfg->format = make_cstr_copy("{album:n/medium:n/tracknumber:02d}_{title:n}.flac");
    cfg->compression_level = -1;
    cfg->max_width = 512;
    cfg->cover_art = make_cstr_copy("auto");
    cfg->accuraterip = true;
    cfg->speed_fast = false;
    cfg->replaygain = true;
    cfg->discogs = make_cstr_copy("always");
    cfg->recrawl_percent = 2;
    cfg->permissions = -1;
    cfg->permission_warnings = true;
    cfg->mode = RIP_MODES_DEFAULT;
    cfg->repeat = false;
    cfg->sort = false;
    cfg->filter_title = nullptr;
    cfg->auto_mode = false;
    cfg->servers = make_builtin_servers();
    cfg->config_path = nullptr;
    return cfg;
}

// All [cdrip] values are interpreted once, before CLI overrides are applied.
static bool apply_config_value(CdRipConfig& cfg, const std::string& key, const std::string& value) {
    const auto normalized = normalized_setting(value);
    if (key == "device") { if (!value.empty()) replace_cstr(cfg.device, value); }
    else if (key == "format") { if (!value.empty()) replace_cstr(cfg.format, value); }
    else if (key == "filter_title") replace_cstr(cfg.filter_title, value);
    else if (key == "compression") return parse_setting_compression(value, cfg.compression_level);
    else if (key == "max_width") return parse_setting_integer(value, cfg.max_width) && cfg.max_width > 0;
    else if (key == "recrawl_percent") return parse_setting_integer(value, cfg.recrawl_percent) && cfg.recrawl_percent >= 0;
    else if (key == "mode") return parse_setting_mode(value, cfg.mode);
    else if (key == "repeat") return parse_setting_boolean(value, cfg.repeat);
    else if (key == "sort") return parse_setting_boolean(value, cfg.sort);
    else if (key == "auto") return parse_setting_boolean(value, cfg.auto_mode);
    else if (key == "accuraterip") return parse_setting_boolean(value, cfg.accuraterip);
    else if (key == "replaygain") return parse_setting_boolean(value, cfg.replaygain);
    else if (key == "permission_warnings") return parse_setting_boolean(value, cfg.permission_warnings);
    else if (key == "speed") {
        if (normalized != "slow" && normalized != "fast") return false;
        cfg.speed_fast = normalized == "fast";
    } else if (key == "cover_art") {
        if (!valid_cover_art_mode(normalized)) return false;
        replace_cstr(cfg.cover_art, normalized);
    } else if (key == "discogs") {
        if (!valid_discogs_mode(normalized)) return false;
        replace_cstr(cfg.discogs, normalized);
    } else if (key == "permissions") {
        unsigned int parsed = 0;
        if (!parse_output_permissions_value(value, parsed)) return false;
        cfg.permissions = static_cast<int>(parsed);
    } else return false;
    return true;
}

/* ------------------------------------------------------------------- */
/* Exported API functions */

extern "C" {

CdRipConfig* cdrip_load_config(
    const char* path,
    const char** error) {

    clear_error(error);

    auto* cfg = make_default_config();
    GKeyFile* key_file = g_key_file_new();
    bool loaded = false;
    std::string loaded_path;
    std::vector<CdRipCddbServer> parsed_servers;

    auto fail = [&](const char* message) -> CdRipConfig* {
        if (message) set_error(error, message);
        release_cddb_servers(parsed_servers);
        cdrip_release_config(cfg);
        g_key_file_unref(key_file);
        return nullptr;
    };

    auto fail_gerror = [&](GError* gerr, const char* fallback) -> CdRipConfig* {
        const char* msg = (gerr && gerr->message) ? gerr->message : fallback;
        if (gerr) g_error_free(gerr);
        return fail(msg);
    };

    std::vector<std::string> candidates;
    if (path) {
        candidates.emplace_back(path);
    } else {
        candidates.emplace_back("cdrip.conf");
        const char* home = std::getenv("HOME");
        if (home) {
            const std::filesystem::path home_path = std::filesystem::path(home) / ".cdrip.conf";
            candidates.emplace_back(home_path.string());
        }
    }

    for (const auto& candidate : candidates) {
        GError* gerr = nullptr;
        if (g_key_file_load_from_file(key_file, candidate.c_str(), G_KEY_FILE_NONE, &gerr)) {
            loaded = true;
            loaded_path = candidate;
            break;
        }
        if (gerr) {
            if (path || gerr->domain != G_FILE_ERROR || gerr->code != G_FILE_ERROR_NOENT) {
                return fail_gerror(gerr, "Failed to load config");
            }
            g_error_free(gerr);
        }
    }

    if (!loaded) {
        g_key_file_unref(key_file);
        return cfg;
    }

    // [cdrip] group
    gsize key_count = 0;
    char** keys = g_key_file_get_keys(key_file, "cdrip", &key_count, nullptr);
    for (gsize i = 0; i < key_count; ++i) {
        const std::string key = keys[i];
        GError* gerr = nullptr;
        char* raw = g_key_file_get_string(key_file, "cdrip", key.c_str(), &gerr);
        if (!raw) {
            g_strfreev(keys);
            return fail_gerror(gerr, "Failed to parse cdrip setting");
        }
        const auto value = strip_inline_comment_value(raw);
        g_free(raw);
        if (!apply_config_value(*cfg, key, value)) {
            g_strfreev(keys);
            return fail(("Invalid " + key + " value").c_str());
        }
    }
    g_strfreev(keys);

    // Device entries are validated even when their drives are disconnected.
    key_count = 0;
    keys = g_key_file_get_keys(key_file, "read_offset", &key_count, nullptr);
    cfg->read_offsets = key_count ? new CdRipReadOffset[key_count]{} : nullptr;
    cfg->read_offsets_count = key_count;
    for (gsize i = 0; i < key_count; ++i) {
        const std::string device = keys[i];
        GError* gerr = nullptr;
        char* raw = g_key_file_get_string(key_file, "read_offset", device.c_str(), &gerr);
        if (!raw) {
            g_strfreev(keys);
            return fail_gerror(gerr, "Failed to parse read offset");
        }
        const auto value = strip_inline_comment_value(raw);
        g_free(raw);
        int samples = 0;
        if (!std::filesystem::path(device).is_absolute() || !parse_setting_integer(value, samples)) {
            g_strfreev(keys);
            return fail(("Invalid read_offset entry: " + device).c_str());
        }
        cfg->read_offsets[i] = {make_cstr_copy(device), samples};
    }
    g_strfreev(keys);

    // [cddb] group: servers list
    std::vector<std::string> server_ids;
    if (g_key_file_has_key(key_file, "cddb", "servers", nullptr)) {
        GError* gerr = nullptr;
        char* value = g_key_file_get_string(key_file, "cddb", "servers", &gerr);
        if (value) {
            server_ids = split_list(strip_inline_comment_value(value));
            g_free(value);
        } else if (gerr) {
            return fail_gerror(gerr, "Failed to parse servers");
        }
    }

    for (const auto& id : server_ids) {
        const std::string group = "cddb." + id;
        // MusicBrainz special entry: allow label only.
        if (to_lower(trim(id)) == "musicbrainz" &&
            !g_key_file_has_group(key_file, group.c_str())) {
            parsed_servers.push_back(
                make_cddb_server("", 80, "", "musicbrainz"));
            continue;
        }

        GError* gerr = nullptr;
        char* host = g_key_file_get_string(key_file, group.c_str(), "host", &gerr);
        if (!host) {
            if (gerr) g_error_free(gerr);
            continue;
        }

        gerr = nullptr;
        char* port_value = g_key_file_get_string(key_file, group.c_str(), "port", &gerr);
        if (!port_value) {
            if (gerr) g_error_free(gerr);
            g_free(host);
            continue;
        }
        int port = 0;
        {
            const std::string v = strip_inline_comment_value(port_value);
            g_free(port_value);
            if (!parse_setting_integer(v, port)) {
                g_free(host);
                continue;
            }
        }

        gerr = nullptr;
        char* path_value = g_key_file_get_string(key_file, group.c_str(), "path", &gerr);
        if (!path_value) {
            if (gerr) g_error_free(gerr);
            g_free(host);
            continue;
        }

        gerr = nullptr;
        char* label_value = g_key_file_get_string(key_file, group.c_str(), "label", &gerr);
        const std::string label = label_value ? strip_inline_comment_value(label_value) : id;
        if (gerr) g_error_free(gerr);

        CdRipCddbServer s = make_cddb_server(
            strip_inline_comment_value(host),
            port,
            strip_inline_comment_value(path_value),
            strip_inline_comment_value(label));

        g_free(host);
        g_free(path_value);
        if (label_value) g_free(label_value);

        parsed_servers.push_back(s);
    }

    if (!parsed_servers.empty()) {
        // Will remove default server list.
        if (cfg->servers) {
            for (size_t i = 0; i < cfg->servers->count; ++i) {
                release_cstr(cfg->servers->servers[i].name);
                release_cstr(cfg->servers->servers[i].path);
                release_cstr(cfg->servers->servers[i].label);
            }
            delete[] cfg->servers->servers;
            delete cfg->servers;
            cfg->servers = nullptr;
        }
        cfg->servers = new CdRipCddbServerList{};
        cfg->servers->count = parsed_servers.size();
        cfg->servers->servers = new CdRipCddbServer[cfg->servers->count]{};
        for (size_t i = 0; i < cfg->servers->count; ++i) {
            cfg->servers->servers[i] = parsed_servers[i];
        }
    }

    g_key_file_unref(key_file);
    if (loaded) {
        const std::string source = loaded_path.empty()
            ? (path ? std::string{path} : std::string{})
            : loaded_path;
        if (!source.empty())
            cfg->config_path = make_cstr_copy(source);
    }
    return cfg;
}

void cdrip_release_config(
    CdRipConfig* cfg) {

    if (!cfg) return;
    release_cstr(cfg->device);
    release_cstr(cfg->format);
    release_cstr(cfg->cover_art);
    release_cstr(cfg->discogs);
    release_cstr(cfg->filter_title);
    release_cstr(cfg->config_path);
    for (size_t i = 0; i < cfg->read_offsets_count; ++i) release_cstr(cfg->read_offsets[i].device);
    delete[] cfg->read_offsets;
    if (cfg->servers) {
        for (size_t i = 0; i < cfg->servers->count; ++i) {
            release_cstr(cfg->servers->servers[i].name);
            release_cstr(cfg->servers->servers[i].path);
            release_cstr(cfg->servers->servers[i].label);
        }
        delete[] cfg->servers->servers;
        delete cfg->servers;
        cfg->servers = nullptr;
    }
    delete cfg;
}

};
