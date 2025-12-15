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

static bool parse_bool_value(
    const std::string& raw,
    bool& out) {

    const std::string value = to_lower(trim(raw));
    if (value == "true" || value == "1") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0") {
        out = false;
        return true;
    }
    return false;
}

static bool parse_int_strict_value(
    const std::string& raw,
    int& out) {

    const std::string value = trim(raw);
    if (value.empty()) return false;

    size_t idx = 0;
    try {
        const int v = std::stoi(value, &idx);
        if (idx != value.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
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

static CdRipRipModes parse_mode(
    const std::string& value) {

    const std::string upper = to_lower(trim(value));
    if (upper == "fast") return RIP_MODES_FAST;
    if (upper == "best") return RIP_MODES_BEST;
    return RIP_MODES_DEFAULT;
}

static CdRipConfig* make_default_config() {
    auto* cfg = new CdRipConfig{};
    cfg->device = nullptr;
    cfg->format = make_cstr_copy("{album}/{tracknumber:02d}_{safetitle}.flac");
    cfg->compression_level = -1;
    cfg->max_width = 512;
    cfg->mode = RIP_MODES_DEFAULT;
    cfg->repeat = false;
    cfg->sort = false;
    cfg->filter_title = nullptr;
    cfg->auto_mode = false;
    cfg->servers = make_builtin_servers();
    cfg->config_path = nullptr;
    return cfg;
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
            if (path) {
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
    if (g_key_file_has_key(key_file, "cdrip", "device", nullptr)) {
        GError* gerr = nullptr;
        char* value = g_key_file_get_string(key_file, "cdrip", "device", &gerr);
        if (value) {
            const std::string device = strip_inline_comment_value(value);
            g_free(value);
            if (!device.empty()) replace_cstr(cfg->device, device);
        } else if (gerr) {
            return fail_gerror(gerr, "Failed to parse device");
        }
    }

    if (g_key_file_has_key(key_file, "cdrip", "format", nullptr)) {
        GError* gerr = nullptr;
        char* value = g_key_file_get_string(key_file, "cdrip", "format", &gerr);
        if (value) {
            const std::string fmt = strip_inline_comment_value(value);
            g_free(value);
            if (!fmt.empty()) replace_cstr(cfg->format, fmt);
        } else if (gerr) {
            return fail_gerror(gerr, "Failed to parse format");
        }
    }

    if (g_key_file_has_key(key_file, "cdrip", "compression", nullptr)) {
        GError* gerr = nullptr;
        char* value = g_key_file_get_string(key_file, "cdrip", "compression", &gerr);
        if (value) {
            const std::string v = strip_inline_comment_value(value);
            g_free(value);
            const std::string upper = to_lower(v);
            if (upper == "auto") {
                cfg->compression_level = -1;
            } else {
                try {
                    cfg->compression_level = std::stoi(v);
                } catch (...) {
                    return fail("Invalid compression value");
                }
            }
        } else if (gerr) {
            return fail_gerror(gerr, "Failed to parse compression");
        }
    }

    if (g_key_file_has_key(key_file, "cdrip", "max_width", nullptr)) {
        GError* gerr = nullptr;
        char* value = g_key_file_get_string(key_file, "cdrip", "max_width", &gerr);
        if (value) {
            int parsed = 0;
            const std::string v = strip_inline_comment_value(value);
            g_free(value);
            if (!parse_int_strict_value(v, parsed) || parsed <= 0) {
                return fail("Invalid max_width value");
            }
            cfg->max_width = parsed;
        } else if (gerr) {
            return fail_gerror(gerr, "Failed to parse max_width");
        }
    }

    if (g_key_file_has_key(key_file, "cdrip", "mode", nullptr)) {
        GError* gerr = nullptr;
        char* value = g_key_file_get_string(key_file, "cdrip", "mode", &gerr);
        if (value) {
            const std::string v = strip_inline_comment_value(value);
            cfg->mode = parse_mode(v);
            g_free(value);
        } else if (gerr) {
            return fail_gerror(gerr, "Failed to parse mode");
        }
    }

    if (g_key_file_has_key(key_file, "cdrip", "repeat", nullptr)) {
        GError* gerr = nullptr;
        char* value = g_key_file_get_string(key_file, "cdrip", "repeat", &gerr);
        if (value) {
            bool parsed = false;
            const std::string v = strip_inline_comment_value(value);
            g_free(value);
            if (!parse_bool_value(v, parsed)) {
                return fail("Invalid repeat value");
            }
            cfg->repeat = parsed;
        } else if (gerr) {
            return fail_gerror(gerr, "Failed to parse repeat");
        }
    }

    if (g_key_file_has_key(key_file, "cdrip", "sort", nullptr)) {
        GError* gerr = nullptr;
        char* value = g_key_file_get_string(key_file, "cdrip", "sort", &gerr);
        if (value) {
            bool parsed = false;
            const std::string v = strip_inline_comment_value(value);
            g_free(value);
            if (!parse_bool_value(v, parsed)) {
                return fail("Invalid sort value");
            }
            cfg->sort = parsed;
        } else if (gerr) {
            return fail_gerror(gerr, "Failed to parse sort");
        }
    }

    if (g_key_file_has_key(key_file, "cdrip", "filter_title", nullptr)) {
        GError* gerr = nullptr;
        char* value = g_key_file_get_string(key_file, "cdrip", "filter_title", &gerr);
        if (value) {
            const std::string v = strip_inline_comment_value(value);
            g_free(value);
            if (!v.empty()) {
                replace_cstr(cfg->filter_title, v);
            } else {
                release_cstr(cfg->filter_title);
                cfg->filter_title = nullptr;
            }
        } else if (gerr) {
            return fail_gerror(gerr, "Failed to parse filter_title");
        }
    }

    if (g_key_file_has_key(key_file, "cdrip", "auto", nullptr)) {
        GError* gerr = nullptr;
        char* value = g_key_file_get_string(key_file, "cdrip", "auto", &gerr);
        if (value) {
            bool parsed = false;
            const std::string v = strip_inline_comment_value(value);
            g_free(value);
            if (!parse_bool_value(v, parsed)) {
                return fail("Invalid auto value");
            }
            cfg->auto_mode = parsed;
        } else if (gerr) {
            return fail_gerror(gerr, "Failed to parse auto");
        }
    }

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
            if (!parse_int_strict_value(v, port)) {
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
    release_cstr(cfg->filter_title);
    release_cstr(cfg->config_path);
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
