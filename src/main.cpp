// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include "cdrip/cdrip.h"
#include "version.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <cctype>
#include <optional>
#include <chrono>
#include <thread>
#include <memory>
#include <sstream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <vector>
#include <map>
#include <unordered_set>

#include <glib.h>

#include <chafa.h>
#include <png.h>

#include <sys/select.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define COVER_ART_AA_WIDTH 35

namespace {

std::string view_string(const char* s) {
    return s ? std::string{s} : std::string{};
}

std::string trim_ws(const std::string& s);

std::string canonicalize_device_path(const std::string& path) {
    if (path.empty()) return {};
    // realpath() resolves symlinks (e.g., /dev/cdrom -> /dev/sr0).
    // If it fails (non-path style device name, permission, etc.), fall back to the original string.
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) return path;
    std::string out = resolved;
    std::free(resolved);
    return out;
}

const char* dup_cstr(const std::string& s) {
    auto* buf = new char[s.size() + 1];
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return buf;
}

const char* dup_cstr(const char* s) {
    return dup_cstr(s ? std::string{s} : std::string{});
}

const char* dup_cstr_nullable(const char* s) {
    return s ? dup_cstr(s) : nullptr;
}

void replace_cstr(const char*& target, const std::string& value) {
    delete[] target;
    target = dup_cstr(value);
}

std::string get_album_tag(
    const CdRipCddbEntry* entry,
    const std::string& key) {

    if (!entry || !entry->album_tags) return {};
    std::string key_upper = key;
    std::transform(key_upper.begin(), key_upper.end(), key_upper.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    for (size_t i = 0; i < entry->album_tags_count; ++i) {
        std::string k = view_string(entry->album_tags[i].key);
        std::string v = view_string(entry->album_tags[i].value);
        std::string ku = k;
        std::transform(ku.begin(), ku.end(), ku.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (ku == key_upper) return v;
    }
    return {};
}

std::string get_album_media_tag(
    const CdRipCddbEntry* entry) {

    const std::string album = trim_ws(get_album_tag(entry, "ALBUM"));

    int disctotal = 0;
    try {
        const std::string raw = trim_ws(get_album_tag(entry, "DISCTOTAL"));
        if (!raw.empty()) disctotal = std::stoi(raw);
    } catch (...) {
        disctotal = 0;
    }
    if (disctotal <= 1) return album;

    const std::string medium_title = trim_ws(get_album_tag(entry, "MUSICBRAINZ_MEDIUMTITLE"));
    if (!medium_title.empty()) {
        if (album.empty()) return medium_title;
        return album + " " + medium_title;
    }

    const std::string discnumber = trim_ws(get_album_tag(entry, "DISCNUMBER"));
    if (discnumber.empty()) return album;
    if (album.empty()) return "CD" + discnumber;
    return album + " CD" + discnumber;
}

[[maybe_unused]] std::string get_track_tag(
    const CdRipCddbEntry* entry,
    size_t index_zero_based,
    const std::string& key) {

    if (!entry || !entry->tracks || index_zero_based >= entry->tracks_count) return {};
    std::string key_upper = key;
    std::transform(key_upper.begin(), key_upper.end(), key_upper.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    const auto& tt = entry->tracks[index_zero_based];
    for (size_t i = 0; i < tt.tags_count; ++i) {
        std::string ku = view_string(tt.tags[i].key);
        std::transform(ku.begin(), ku.end(), ku.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (ku == key_upper) return view_string(tt.tags[i].value);
    }
    return {};
}

std::string fmt_time_fn(double sec) {
    if (sec < 0) sec = 0;
    int t = static_cast<int>(sec + 0.5);
    int m = t / 60;
    int s = t % 60;
    std::ostringstream os;
    os << std::setw(2) << std::setfill('0') << m << ":"
       << std::setw(2) << std::setfill('0') << s;
    return os.str();
}

void progress_cb(const CdRipProgressInfo* info) {
    if (!info) return;
    // Avoid noisy ETA early in the track: wait for minimal progress/time.
    constexpr double kMinElapsedSec = 10.0;  // wall-clock seconds from album start
    bool show_eta = info->wall_elapsed_sec >= kMinElapsedSec;

    double remaining_total = info->wall_total_sec > 0.0
        ? info->wall_total_sec - info->wall_elapsed_sec
        : info->total_album_sec - info->elapsed_total_sec;
    if (remaining_total < 0) remaining_total = 0;
    const int bar_width = 20;
    int filled = static_cast<int>(info->percent / 100.0 * bar_width);
    if (filled > bar_width) filled = bar_width;
    std::string bar(filled, '=');
    if (filled < bar_width) {
        bar.push_back('>');
        bar.append(bar_width - filled - 1, '-');
    }
    std::string track_name = view_string(info->track_name);
    if (track_name.empty()) track_name = view_string(info->title);
    std::cout << "\rTrack " << std::setw(2) << info->track_number << "/" << std::setw(2) << info->total_tracks
              << " [ETA: " << (show_eta ? fmt_time_fn(remaining_total) : "--:--") << " " << bar << "]: "
              << "\"" << track_name << "\"";
    std::cout.flush();
        if (info->percent >= 100.0) std::cout << "\n";
}

struct TerminalSize {
    int columns{0};
    int rows{0};
};

std::string strip_inline_comment_value(
    const std::string& raw) {

    auto trim_ws_local = [](const std::string& s) {
        const size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return std::string{};
        const size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    };

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
                return trim_ws_local(raw.substr(0, i));
            }
        }
    }
    return trim_ws_local(raw);
}

bool parse_bool_value(
    const std::string& raw,
    bool& out) {

    std::string value = strip_inline_comment_value(raw);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
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

bool get_config_bool(
    const char* config_path,
    const char* group,
    const char* key,
    bool default_value,
    std::string& err_out) {

    err_out.clear();
    if (!config_path || !group || !key) return default_value;

    GKeyFile* key_file = g_key_file_new();
    GError* gerr = nullptr;
    if (!g_key_file_load_from_file(key_file, config_path, G_KEY_FILE_NONE, &gerr)) {
        err_out = (gerr && gerr->message) ? gerr->message : "Failed to load config file";
        if (gerr) g_error_free(gerr);
        g_key_file_unref(key_file);
        return default_value;
    }

    bool value = default_value;
    if (g_key_file_has_key(key_file, group, key, nullptr)) {
        gerr = nullptr;
        char* raw = g_key_file_get_string(key_file, group, key, &gerr);
        if (!raw) {
            err_out = (gerr && gerr->message) ? gerr->message : "Failed to parse boolean value";
            if (gerr) g_error_free(gerr);
            g_key_file_unref(key_file);
            return default_value;
        }
        bool parsed = false;
        const std::string cleaned = raw;
        g_free(raw);
        if (!parse_bool_value(cleaned, parsed)) {
            err_out = "Failed to parse boolean value";
            g_key_file_unref(key_file);
            return default_value;
        }
        value = parsed;
    }

    g_key_file_unref(key_file);
    return value;
}

std::string get_config_string(
    const char* config_path,
    const char* group,
    const char* key,
    const char* default_value,
    std::string& err_out) {

    err_out.clear();
    if (!config_path || !group || !key) return default_value ? std::string{default_value} : std::string{};

    GKeyFile* key_file = g_key_file_new();
    GError* gerr = nullptr;
    if (!g_key_file_load_from_file(key_file, config_path, G_KEY_FILE_NONE, &gerr)) {
        err_out = (gerr && gerr->message) ? gerr->message : "Failed to load config file";
        if (gerr) g_error_free(gerr);
        g_key_file_unref(key_file);
        return default_value ? std::string{default_value} : std::string{};
    }

    std::string value = default_value ? std::string{default_value} : std::string{};
    if (g_key_file_has_key(key_file, group, key, nullptr)) {
        gerr = nullptr;
        char* raw = g_key_file_get_string(key_file, group, key, &gerr);
        if (!raw) {
            err_out = (gerr && gerr->message) ? gerr->message : "Failed to parse string value";
            if (gerr) g_error_free(gerr);
            g_key_file_unref(key_file);
            return default_value ? std::string{default_value} : std::string{};
        }
        value = strip_inline_comment_value(raw);
        g_free(raw);
    }

    g_key_file_unref(key_file);
    return value;
}

TerminalSize get_stdout_terminal_size() {
    TerminalSize out{};
    struct winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        out.columns = static_cast<int>(ws.ws_col);
        out.rows = static_cast<int>(ws.ws_row);
    }

    auto parse_env_int = [](const char* value) -> int {
        if (!value || *value == '\0') return 0;
        char* end = nullptr;
        errno = 0;
        const long v = std::strtol(value, &end, 10);
        if (errno != 0 || end == value || v <= 0 || v > 10000) return 0;
        return static_cast<int>(v);
    };

    if (out.columns <= 0) out.columns = parse_env_int(std::getenv("COLUMNS"));
    if (out.rows <= 0) out.rows = parse_env_int(std::getenv("LINES"));
    return out;
}

int compute_cover_art_columns_limit(int tty_columns) {
    //const int cols = tty_columns > 0 ? tty_columns : COVER_ART_AA_WIDTH;
    //const int eighty_percent = (cols * 80) / 100;
    //return std::max(1, std::min(COVER_ART_AA_WIDTH, eighty_percent));
    (void)tty_columns;
    return COVER_ART_AA_WIDTH;
}

bool decode_png_to_rgba(
    const uint8_t* data,
    size_t size,
    int& width_out,
    int& height_out,
    std::vector<uint8_t>& rgba_out,
    std::string& err_out) {

    width_out = 0;
    height_out = 0;
    rgba_out.clear();
    err_out.clear();

    if (!data || size == 0) {
        err_out = "PNG input is empty";
        return false;
    }

    png_image image{};
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(&image, data, size)) {
        err_out = image.message[0] ? image.message : "png_image_begin_read_from_memory failed";
        png_image_free(&image);
        return false;
    }

    image.format = PNG_FORMAT_RGBA;
    width_out = static_cast<int>(image.width);
    height_out = static_cast<int>(image.height);
    if (width_out <= 0 || height_out <= 0) {
        err_out = "Invalid PNG geometry";
        png_image_free(&image);
        return false;
    }

    rgba_out.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, rgba_out.data(), 0, nullptr)) {
        err_out = image.message[0] ? image.message : "png_image_finish_read failed";
        png_image_free(&image);
        rgba_out.clear();
        width_out = 0;
        height_out = 0;
        return false;
    }

    png_image_free(&image);
    return true;
}

void maybe_print_cover_art_ascii(const CdRipCoverArt& art) {
    if (!art.data || art.size == 0) return;
    if (::isatty(STDOUT_FILENO) == 0) return;

    int img_w = 0;
    int img_h = 0;
    std::vector<uint8_t> rgba;
    std::string decode_err;
    if (!decode_png_to_rgba(art.data, art.size, img_w, img_h, rgba, decode_err)) {
        return;
    }

    const TerminalSize tty = get_stdout_terminal_size();
    const int max_cols = compute_cover_art_columns_limit(tty.columns);

    gint canvas_cols = max_cols;
    gint canvas_rows = -1;
    chafa_calc_canvas_geometry(
        img_w,
        img_h,
        &canvas_cols,
        &canvas_rows,
        0.5f,
        TRUE,
        FALSE);
    if (canvas_cols <= 0) canvas_cols = 1;
    if (canvas_rows <= 0) canvas_rows = 1;

    ChafaCanvasConfig* config = chafa_canvas_config_new();
    chafa_canvas_config_set_canvas_mode(config, CHAFA_CANVAS_MODE_TRUECOLOR);
    chafa_canvas_config_set_dither_mode(config, CHAFA_DITHER_MODE_DIFFUSION);
    chafa_canvas_config_set_geometry(config, canvas_cols, canvas_rows);

    ChafaSymbolMap* symbols = chafa_symbol_map_new();
    const auto tags = static_cast<ChafaSymbolTags>(CHAFA_SYMBOL_TAG_ASCII | CHAFA_SYMBOL_TAG_SPACE);
    chafa_symbol_map_add_by_tags(symbols, tags);
    chafa_canvas_config_set_symbol_map(config, symbols);
    chafa_symbol_map_unref(symbols);

    ChafaCanvas* canvas = chafa_canvas_new(config);
    chafa_canvas_config_unref(config);
    if (!canvas) return;

    chafa_canvas_draw_all_pixels(
        canvas,
        CHAFA_PIXEL_RGBA8_UNASSOCIATED,
        rgba.data(),
        img_w,
        img_h,
        img_w * 4);

    ChafaTermDb* term_db = chafa_term_db_get_default();
    gchar** envp = g_get_environ();
    ChafaTermInfo* term_info = chafa_term_db_detect(term_db, envp);
    g_strfreev(envp);
    if (!term_info) term_info = chafa_term_db_get_fallback_info(term_db);
    if (!term_info) {
        chafa_canvas_unref(canvas);
        return;
    }

    GString* out = chafa_canvas_print(canvas, term_info);
    if (out) {
        std::cout << "\n" << out->str << "\x1b[0m\n";
        g_string_free(out, TRUE);
    }

    chafa_term_info_unref(term_info);
    chafa_canvas_unref(canvas);
}

CdRipCoverArt clone_cover_art(const CdRipCoverArt& src) {
    CdRipCoverArt dest{};
    dest.size = src.size;
    dest.is_front = src.is_front;
    dest.available = src.available;
    dest.mime_type = dup_cstr_nullable(src.mime_type);
    if (src.data && src.size > 0) {
        auto* buffer = new uint8_t[src.size];
        std::memcpy(buffer, src.data, src.size);
        dest.data = buffer;
    }
    return dest;
}

CdRipTagKV clone_kv(const CdRipTagKV& kv) {
    CdRipTagKV dest{};
    dest.key = dup_cstr_nullable(kv.key);
    dest.value = dup_cstr_nullable(kv.value);
    return dest;
}

CdRipTrackTags clone_track_tags(const CdRipTrackTags& src) {
    CdRipTrackTags dest{};
    dest.tags_count = src.tags_count;
    if (src.tags && src.tags_count > 0) {
        dest.tags = new CdRipTagKV[src.tags_count]{};
        for (size_t i = 0; i < src.tags_count; ++i) {
            dest.tags[i] = clone_kv(src.tags[i]);
        }
    }
    return dest;
}

CdRipCddbEntry clone_cddb_entry(const CdRipCddbEntry& src) {
    CdRipCddbEntry dest{};
    dest.cddb_discid = dup_cstr_nullable(src.cddb_discid);
    dest.source_label = dup_cstr_nullable(src.source_label);
    dest.source_url = dup_cstr_nullable(src.source_url);
    dest.fetched_at = dup_cstr_nullable(src.fetched_at);

    dest.album_tags_count = src.album_tags_count;
    if (src.album_tags && src.album_tags_count > 0) {
        dest.album_tags = new CdRipTagKV[src.album_tags_count]{};
        for (size_t i = 0; i < src.album_tags_count; ++i) {
            dest.album_tags[i] = clone_kv(src.album_tags[i]);
        }
    }

    dest.tracks_count = src.tracks_count;
    if (src.tracks && src.tracks_count > 0) {
        dest.tracks = new CdRipTrackTags[src.tracks_count]{};
        for (size_t i = 0; i < src.tracks_count; ++i) {
            dest.tracks[i] = clone_track_tags(src.tracks[i]);
        }
    }

    dest.cover_art = clone_cover_art(src.cover_art);
    return dest;
}

CdRipCddbEntryList* clone_cddb_entry_list(const CdRipCddbEntryList* src) {
    if (!src) return nullptr;
    auto* list = new CdRipCddbEntryList{};
    list->count = src->count;
    if (list->count > 0 && src->entries) {
        list->entries = new CdRipCddbEntry[list->count]{};
        for (size_t i = 0; i < list->count; ++i) {
            list->entries[i] = clone_cddb_entry(src->entries[i]);
        }
    }
    return list;
}

std::string build_musicbrainz_cache_key(const CdRipDiscToc* toc) {
    if (!toc) return {};
    const std::string discid = view_string(toc->mb_discid);
    if (!discid.empty()) {
        return "mb:discid:" + discid;
    }
    const std::string release_id = view_string(toc->mb_release_id);
    const std::string medium_id = view_string(toc->mb_medium_id);
    if (!release_id.empty()) {
        if (!medium_id.empty()) {
            return "mb:release:" + release_id + "|medium:" + medium_id;
        }
        return "mb:release:" + release_id;
    }
    return {};
}

std::string build_metadata_cache_key(const CdRipDiscToc* toc) {
    const std::string mb_key = build_musicbrainz_cache_key(toc);
    if (!mb_key.empty()) return mb_key;

    const std::string cddb = view_string(toc ? toc->cddb_discid : nullptr);
    if (!cddb.empty()) {
        std::string lowered = cddb;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return "cddb:" + lowered;
    }
    return {};
}

struct EntryListDeleter {
    void operator()(CdRipCddbEntryList* p) const {
        cdrip_release_cddbentry_list(p);
    }
};

using EntryListPtr = std::unique_ptr<CdRipCddbEntryList, EntryListDeleter>;
using EntryListCache = std::map<std::string, EntryListPtr>;

void ensure_entry_ready_for_toc(
    CdRipCddbEntry* entry,
    const CdRipDiscToc* toc,
    bool fill_timestamp = true) {

    if (!entry || !toc) return;
    std::string fetched_at = view_string(entry->fetched_at);
    if (fill_timestamp && fetched_at.empty()) {
        char* ts = cdrip_current_timestamp_iso();
        replace_cstr(entry->fetched_at, ts);
        cdrip_release_timestamp(ts);
    }

    size_t needed_tracks = toc->tracks_count;
    std::vector<CdRipTrackTags> rebuilt(needed_tracks);
    for (size_t i = 0; i < needed_tracks; ++i) {
        std::vector<CdRipTagKV> kvs;
        if (entry->tracks && i < entry->tracks_count) {
            const auto& tt = entry->tracks[i];
            for (size_t k = 0; k < tt.tags_count; ++k) {
                kvs.push_back(tt.tags[k]);
            }
        }
        bool has_title = false;
        for (const auto& kv : kvs) {
            std::string key = kv.key ? std::string{kv.key} : std::string{};
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            if (key == "TITLE" && kv.value && std::strlen(kv.value) > 0) {
                has_title = true;
                break;
            }
        }
        if (!has_title) {
            std::ostringstream oss;
            oss << "Track " << (i + 1);
            CdRipTagKV kv{};
            kv.key = dup_cstr("TITLE");
            kv.value = dup_cstr(oss.str());
            kvs.push_back(kv);
        }
        if (!kvs.empty()) {
            rebuilt[i].tags_count = kvs.size();
            rebuilt[i].tags = new CdRipTagKV[kvs.size()]{};
            for (size_t k = 0; k < kvs.size(); ++k) {
                rebuilt[i].tags[k] = kvs[k];
            }
        }
    }

    if (entry->tracks) {
        // We rebuild the track tag arrays below, reusing the underlying C strings.
        // Free only the old tag arrays here to avoid leaking them (strings are freed later).
        for (size_t i = 0; i < entry->tracks_count; ++i) {
            delete[] entry->tracks[i].tags;
            entry->tracks[i].tags = nullptr;
            entry->tracks[i].tags_count = 0;
        }
        delete[] entry->tracks;
    }
    entry->tracks_count = rebuilt.size();
    if (!rebuilt.empty()) {
        entry->tracks = new CdRipTrackTags[rebuilt.size()]{};
        for (size_t i = 0; i < rebuilt.size(); ++i) {
            entry->tracks[i] = rebuilt[i];
        }
    } else {
        entry->tracks = nullptr;
    }
}

CdRipCddbEntry* make_fallback_entry(const CdRipDiscToc* toc) {
    if (!toc) return nullptr;
    CdRipCddbEntry* entry = new CdRipCddbEntry{};
    std::string discid = view_string(toc->cddb_discid);
    if (discid.empty()) discid = "unknown";
    entry->cddb_discid = dup_cstr(discid);
    entry->source_label = dup_cstr("none");
    entry->source_url = dup_cstr("");
    char* ts = cdrip_current_timestamp_iso();
    entry->fetched_at = dup_cstr(ts);
    cdrip_release_timestamp(ts);
    entry->album_tags_count = 4;
    entry->album_tags = new CdRipTagKV[entry->album_tags_count]{
        {dup_cstr("ARTIST"), dup_cstr("")},
        {dup_cstr("ALBUM"), dup_cstr("")},
        {dup_cstr("GENRE"), dup_cstr("")},
        {dup_cstr("DATE"), dup_cstr("")},
    };
    entry->tracks_count = toc->tracks_count;
    if (entry->tracks_count > 0) entry->tracks = new CdRipTrackTags[entry->tracks_count]{};
    for (size_t i = 0; i < toc->tracks_count; ++i) {
        std::ostringstream oss;
        oss << "Track " << (i + 1);
        entry->tracks[i].tags_count = 1;
        entry->tracks[i].tags = new CdRipTagKV[1]{
            {dup_cstr("TITLE"), dup_cstr(oss.str())},
        };
    }
    return entry;
}

std::string to_lower_ascii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

std::string to_upper_ascii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        out.push_back(static_cast<char>(std::toupper(ch)));
    }
    return out;
}

std::string trim_ws(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

enum class DiscogsMode {
    No,
    Always,
    Fallback,
};

bool parse_discogs_mode(
    const std::string& raw,
    DiscogsMode& out) {

    std::string value = to_lower_ascii(trim_ws(raw));
    if (value.empty()) value = "always";
    if (value == "no") {
        out = DiscogsMode::No;
        return true;
    }
    if (value == "always") {
        out = DiscogsMode::Always;
        return true;
    }
    if (value == "fallback") {
        out = DiscogsMode::Fallback;
        return true;
    }
    return false;
}

const char* discogs_mode_label(DiscogsMode mode) {
    switch (mode) {
        case DiscogsMode::No: return "no";
        case DiscogsMode::Always: return "always";
        case DiscogsMode::Fallback: return "fallback";
    }
    return "unknown";
}

bool servers_include_musicbrainz(
    const CdRipCddbServerList* servers) {

    if (!servers || !servers->servers || servers->count == 0) return false;
    for (size_t i = 0; i < servers->count; ++i) {
        const std::string label = to_lower_ascii(view_string(servers->servers[i].label));
        if (label == "musicbrainz") return true;
    }
    return false;
}

enum class CoverArtFetchSource {
    None,
    CoverArtArchive,
    Discogs,
};

const char* cover_art_source_label(CoverArtFetchSource src) {
    switch (src) {
        case CoverArtFetchSource::None: return "none";
        case CoverArtFetchSource::CoverArtArchive: return "Cover Art Archive";
        case CoverArtFetchSource::Discogs: return "Discogs";
    }
    return "unknown";
}

bool is_multi_value_tag_key(const std::string& key_upper) {
    // Tags that may contain multiple values separated by ',' or ';'.
    // e.g. GENRE: "foo; bar" / ISRC: "AAA; BBB"
    return key_upper == "GENRE" || key_upper == "ISRC";
}

std::vector<std::string> split_multi_values(const std::string& raw) {
    std::vector<std::string> out;
    std::string token;
    token.reserve(raw.size());
    auto flush = [&]() {
        std::string t = trim_ws(token);
        if (!t.empty()) out.push_back(std::move(t));
        token.clear();
    };
    for (char ch : raw) {
        if (ch == ',' || ch == ';') {
            flush();
            continue;
        }
        token.push_back(ch);
    }
    flush();
    return out;
}

std::string join_multi_values(const std::vector<std::string>& values) {
    std::ostringstream oss;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) oss << ";";
        oss << values[i];
    }
    return oss.str();
}

std::string merge_multi_values_zip(
    const std::vector<std::vector<std::string>>& per_entry_tokens) {

    size_t max_len = 0;
    for (const auto& tokens : per_entry_tokens) {
        max_len = std::max(max_len, tokens.size());
    }

    std::vector<std::string> merged;
    merged.reserve(max_len * (per_entry_tokens.empty() ? 0 : per_entry_tokens.size()));
    std::unordered_set<std::string> seen;
    for (size_t pos = 0; pos < max_len; ++pos) {
        for (const auto& tokens : per_entry_tokens) {
            if (pos >= tokens.size()) continue;
            const std::string t = trim_ws(tokens[pos]);
            if (t.empty()) continue;
            const std::string norm = to_lower_ascii(t);
            if (seen.insert(norm).second) {
                merged.push_back(t);
            }
        }
    }

    return join_multi_values(merged);
}

CdRipCddbEntryList* merge_cddb_entries_for_toc(
    const CdRipDiscToc* toc,
    const std::vector<CdRipCddbEntry*>& selected_entries) {

    if (!toc || selected_entries.empty()) return nullptr;

    CdRipCddbEntry merged{};

    auto pick_first_nonempty = [&](auto selector) -> std::string {
        for (const auto* e : selected_entries) {
            if (!e) continue;
            const std::string v = trim_ws(selector(*e));
            if (!v.empty()) return v;
        }
        return {};
    };

    std::string discid = pick_first_nonempty([](const CdRipCddbEntry& e) { return view_string(e.cddb_discid); });
    if (discid.empty()) discid = trim_ws(view_string(toc->cddb_discid));
    if (discid.empty()) discid = "unknown";
    merged.cddb_discid = dup_cstr(discid);

    const std::string source_label = pick_first_nonempty([](const CdRipCddbEntry& e) { return view_string(e.source_label); });
    const std::string source_url = pick_first_nonempty([](const CdRipCddbEntry& e) { return view_string(e.source_url); });
    const std::string fetched_at = pick_first_nonempty([](const CdRipCddbEntry& e) { return view_string(e.fetched_at); });
    merged.source_label = dup_cstr(source_label);
    merged.source_url = dup_cstr(source_url);
    merged.fetched_at = dup_cstr(fetched_at);

    std::map<std::string, std::string> merged_album_tags;
    std::unordered_set<std::string> album_keys_seen;
    for (const auto* e : selected_entries) {
        if (!e || !e->album_tags) continue;
        for (size_t i = 0; i < e->album_tags_count; ++i) {
            const std::string key_upper = to_upper_ascii(view_string(e->album_tags[i].key));
            if (key_upper.empty() || is_multi_value_tag_key(key_upper)) continue;
            if (album_keys_seen.find(key_upper) != album_keys_seen.end()) continue;
            const std::string value = trim_ws(view_string(e->album_tags[i].value));
            if (value.empty()) continue;
            merged_album_tags[key_upper] = value;
            album_keys_seen.insert(key_upper);
        }
    }
    for (const char* multi_key : {"GENRE", "ISRC"}) {
        std::vector<std::vector<std::string>> per_entry_tokens;
        per_entry_tokens.reserve(selected_entries.size());
        for (const auto* e : selected_entries) {
            std::vector<std::string> tokens;
            if (e && e->album_tags) {
                for (size_t i = 0; i < e->album_tags_count; ++i) {
                    const std::string key_upper = to_upper_ascii(view_string(e->album_tags[i].key));
                    if (key_upper != multi_key) continue;
                    const std::string value = view_string(e->album_tags[i].value);
                    auto parts = split_multi_values(value);
                    tokens.insert(tokens.end(), parts.begin(), parts.end());
                }
            }
            per_entry_tokens.push_back(std::move(tokens));
        }
        const std::string merged_value = merge_multi_values_zip(per_entry_tokens);
        if (!merged_value.empty()) {
            merged_album_tags[multi_key] = merged_value;
        }
    }

    if (!merged_album_tags.empty()) {
        merged.album_tags_count = merged_album_tags.size();
        merged.album_tags = new CdRipTagKV[merged.album_tags_count]{};
        size_t idx = 0;
        for (const auto& [key, value] : merged_album_tags) {
            merged.album_tags[idx].key = dup_cstr(key);
            merged.album_tags[idx].value = dup_cstr(value);
            ++idx;
        }
    }

    const size_t tracks = toc->tracks_count;
    merged.tracks_count = tracks;
    if (tracks > 0) {
        merged.tracks = new CdRipTrackTags[tracks]{};
    }

    for (size_t ti = 0; ti < tracks; ++ti) {
        std::map<std::string, std::string> merged_track_tags;
        std::unordered_set<std::string> track_keys_seen;
        for (const auto* e : selected_entries) {
            if (!e || !e->tracks || ti >= e->tracks_count) continue;
            const auto& tt = e->tracks[ti];
            if (!tt.tags) continue;
            for (size_t k = 0; k < tt.tags_count; ++k) {
                const std::string key_upper = to_upper_ascii(view_string(tt.tags[k].key));
                if (key_upper.empty() || is_multi_value_tag_key(key_upper)) continue;
                if (track_keys_seen.find(key_upper) != track_keys_seen.end()) continue;
                const std::string value = trim_ws(view_string(tt.tags[k].value));
                if (value.empty()) continue;
                merged_track_tags[key_upper] = value;
                track_keys_seen.insert(key_upper);
            }
        }
        for (const char* multi_key : {"GENRE", "ISRC"}) {
            std::vector<std::vector<std::string>> per_entry_tokens;
            per_entry_tokens.reserve(selected_entries.size());
            for (const auto* e : selected_entries) {
                std::vector<std::string> tokens;
                if (e && e->tracks && ti < e->tracks_count) {
                    const auto& tt = e->tracks[ti];
                    if (tt.tags) {
                        for (size_t k = 0; k < tt.tags_count; ++k) {
                            const std::string key_upper = to_upper_ascii(view_string(tt.tags[k].key));
                            if (key_upper != multi_key) continue;
                            const std::string value = view_string(tt.tags[k].value);
                            auto parts = split_multi_values(value);
                            tokens.insert(tokens.end(), parts.begin(), parts.end());
                        }
                    }
                }
                per_entry_tokens.push_back(std::move(tokens));
            }
            const std::string merged_value = merge_multi_values_zip(per_entry_tokens);
            if (!merged_value.empty()) {
                merged_track_tags[multi_key] = merged_value;
            }
        }

        if (!merged_track_tags.empty()) {
            merged.tracks[ti].tags_count = merged_track_tags.size();
            merged.tracks[ti].tags = new CdRipTagKV[merged.tracks[ti].tags_count]{};
            size_t idx = 0;
            for (const auto& [key, value] : merged_track_tags) {
                merged.tracks[ti].tags[idx].key = dup_cstr(key);
                merged.tracks[ti].tags[idx].value = dup_cstr(value);
                ++idx;
            }
        }
    }

    auto* out = new CdRipCddbEntryList{};
    out->count = 1;
    out->entries = new CdRipCddbEntry[1]{};
    out->entries[0] = merged;
    return out;
}

void clear_cover_art(CdRipCoverArt& art) {
    if (art.data) {
        delete[] art.data;
        art.data = nullptr;
    }
    art.size = 0;
    if (art.mime_type) {
        delete[] art.mime_type;
        art.mime_type = nullptr;
    }
    art.is_front = 0;
    art.available = 0;
}

bool ensure_cover_art_merged(
    CdRipCddbEntry* target,
    const std::vector<CdRipCddbEntry*>& candidates,
    const CdRipDiscToc* toc,
    DiscogsMode discogs_mode,
    CoverArtFetchSource& source_out,
    std::string& notice_out,
    bool allow_aa) {

    notice_out.clear();
    source_out = CoverArtFetchSource::None;
    if (!target) return false;
    const bool target_has_cover = (target->cover_art.data && target->cover_art.size > 0);
    if (target_has_cover && discogs_mode != DiscogsMode::Always) return true;

    const std::vector<CdRipCddbEntry*> effective =
        !candidates.empty() ? candidates : std::vector<CdRipCddbEntry*>{target};

    struct PhaseResult {
        bool success{false};
        bool had_error{false};
    };

    auto try_phase = [&](auto fetch_fn, CoverArtFetchSource phase_source) -> PhaseResult {
        PhaseResult result{};
        for (CdRipCddbEntry* e : effective) {
            if (!e) continue;
            const bool had_data = (e->cover_art.data && e->cover_art.size > 0);
            const char* cover_err = nullptr;
            const int ok = fetch_fn(e, toc, &cover_err);
            if (ok && e->cover_art.data && e->cover_art.size > 0) {
                if (e != target) {
                    clear_cover_art(target->cover_art);
                    target->cover_art = clone_cover_art(e->cover_art);
                }
                if (allow_aa && !had_data) {
                    maybe_print_cover_art_ascii(target->cover_art);
                }
                if (!had_data) {
                    source_out = phase_source;
                }
                if (cover_err) cdrip_release_error(cover_err);
                result.success = true;
                return result;
            }
            if (cover_err) {
                notice_out = view_string(cover_err);
                result.had_error = true;
                cdrip_release_error(cover_err);
            }
        }
        return result;
    };

    if (discogs_mode == DiscogsMode::Always) {
        PhaseResult discogs_result = try_phase(&cdrip_fetch_discogs_cover_art, CoverArtFetchSource::Discogs);
        if (discogs_result.success) return true;
        // Keep any existing cover art if Discogs did not succeed.
        if (target_has_cover) return true;
        PhaseResult caa_result = try_phase(&cdrip_fetch_cover_art, CoverArtFetchSource::CoverArtArchive);
        if (caa_result.success) return true;
        if (caa_result.had_error) {
            PhaseResult retry_discogs = try_phase(&cdrip_fetch_discogs_cover_art, CoverArtFetchSource::Discogs);
            if (retry_discogs.success) return true;
        }
        return false;
    }
    if (discogs_mode == DiscogsMode::Fallback) {
        PhaseResult caa_result = try_phase(&cdrip_fetch_cover_art, CoverArtFetchSource::CoverArtArchive);
        if (caa_result.success) return true;
        PhaseResult discogs_result = try_phase(&cdrip_fetch_discogs_cover_art, CoverArtFetchSource::Discogs);
        if (discogs_result.success) return true;
        if (discogs_result.had_error) {
            PhaseResult retry_caa = try_phase(&cdrip_fetch_cover_art, CoverArtFetchSource::CoverArtArchive);
            if (retry_caa.success) return true;
        }
        return false;
    }
    return try_phase(&cdrip_fetch_cover_art, CoverArtFetchSource::CoverArtArchive).success;
}

struct CddbSelection {
    CdRipCddbEntryList* entries{nullptr};
    CdRipCddbEntryList* merged{nullptr};
    CdRipCddbEntry* selected{nullptr};
    std::vector<CdRipCddbEntry*> selected_entries{};
    bool ignored{false};
};

CddbSelection select_cddb_entry_for_toc(
    const CdRipDiscToc* toc,
    const CdRipCddbServerList* servers,
    bool sort,
    const std::string& context_label = std::string{},
    bool auto_mode = false,
    bool allow_fallback = true,
    EntryListCache* metadata_cache = nullptr,
    const GRegex* title_filter = nullptr) {

    CddbSelection result{};
    if (!toc || !servers) return result;

    if (!context_label.empty()) {
        std::cout << "\nTarget: " << context_label << "\n";
    }
    std::string toc_discid = view_string(toc->cddb_discid);
    std::cout << "CDDB disc id: \"" << (toc_discid.empty() ? "unknown" : toc_discid) << "\"\n";
    std::string toc_mb_discid = view_string(toc->mb_discid);
    std::string toc_mb_release = view_string(toc->mb_release_id);
    std::string toc_mb_medium = view_string(toc->mb_medium_id);
    if (!toc_mb_discid.empty()) {
        std::cout << "MusicBrainz disc id: \"" << toc_mb_discid << "\"\n";
    } else if (!toc_mb_release.empty() || !toc_mb_medium.empty()) {
        if (!toc_mb_release.empty()) {
            std::cout << "MusicBrainz release id: \"" << toc_mb_release << "\"\n";
        }
        if (!toc_mb_medium.empty()) {
            std::cout << "MusicBrainz medium id: \"" << toc_mb_medium << "\"\n";
        }
    } else {
        std::cout << "MusicBrainz disc id: \"unknown\"\n";
    }
    std::cout << "\n";

    std::cout << "Fetcing music tags from servers ...\n";
    const char* fetch_err = nullptr;
    CdRipCddbEntryList* entries = nullptr;
    std::string cache_key;
    if (metadata_cache) {
        cache_key = build_metadata_cache_key(toc);
        if (!cache_key.empty()) {
            auto it = metadata_cache->find(cache_key);
            if (it != metadata_cache->end() && it->second) {
                entries = clone_cddb_entry_list(it->second.get());
            }
        }
    }
    if (!entries) {
        entries = cdrip_fetch_cddb_entries(toc, servers, &fetch_err);
        if (entries && metadata_cache && !cache_key.empty()) {
            (*metadata_cache)[cache_key] = EntryListPtr(
                clone_cddb_entry_list(entries));
        }
    }
    std::cout << "\n";
    if (fetch_err) {
        std::cerr << "CDDB fetch notice: " << view_string(fetch_err) << "\n";
        cdrip_release_error(fetch_err);
        fetch_err = nullptr;
    }

    const size_t fetched_count = entries ? entries->count : 0;

    std::vector<size_t> sorted_indices;
    sorted_indices.reserve(fetched_count);

    if (entries && entries->count > 0) {
        for (size_t i = 0; i < entries->count; ++i) {
            const auto& e = entries->entries[i];
            const std::string title = get_album_media_tag(&e);
            if (!title_filter || g_regex_match(title_filter, title.c_str(), static_cast<GRegexMatchFlags>(0), nullptr)) {
                sorted_indices.push_back(i);
            }
        }
    }

    const bool had_candidates = !sorted_indices.empty();
    if (title_filter && fetched_count > 0) {
        const char* pattern = g_regex_get_pattern(title_filter);
        std::cout << "Title filter: \"" << view_string(pattern) << "\" --> " << sorted_indices.size()
                  << "/" << fetched_count << " candidate(s)\n";
    }

    if (!had_candidates && !allow_fallback) {
        if (title_filter && fetched_count > 0) {
            std::cerr << "No CDDB matches matched the title filter; skipping metadata selection\n";
        } else {
            std::cerr << "No CDDB matches found across configured servers; skipping metadata selection\n";
        }
        result.entries = entries;
        result.selected = nullptr;
        result.ignored = true;
        return result;
    }
    const bool had_candidates_before_fallback = had_candidates;
    if (!had_candidates) {
        if (title_filter && fetched_count > 0) {
            std::cerr << "No CDDB matches matched the title filter; using fallback metadata\n";
        } else {
            std::cerr << "No CDDB matches found across configured servers; using fallback metadata\n";
        }
        cdrip_release_cddbentry_list(entries);
        entries = new CdRipCddbEntryList{};
        entries->count = 1;
        entries->entries = new CdRipCddbEntry[1]{};
        CdRipCddbEntry* fallback = make_fallback_entry(toc);
        if (fallback) {
            entries->entries[0] = *fallback;
            delete fallback;  // ownership transferred to entries->entries
        }
        sorted_indices.assign(1, 0);
    }

    if (sort) {
        auto normalize_lower = [](const std::string& s) {
            std::string lowered = s;
            std::transform(
                lowered.begin(), lowered.end(), lowered.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return lowered;
        };
        std::sort(sorted_indices.begin(), sorted_indices.end(), [&](size_t lhs, size_t rhs) {
            const auto& l = entries->entries[lhs];
            const auto& r = entries->entries[rhs];
            std::string la = normalize_lower(get_album_media_tag(&l));
            std::string ra = normalize_lower(get_album_media_tag(&r));
            if (la == ra) {
                return normalize_lower(get_album_tag(&l, "ARTIST")) < normalize_lower(get_album_tag(&r, "ARTIST"));
            }
            return la < ra;
        });
    }
    auto has_cover_art = [](const CdRipCddbEntry& e) {
        std::string source = view_string(e.source_label);
        std::string source_lower = source;
        std::transform(source_lower.begin(), source_lower.end(), source_lower.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return (source_lower == "musicbrainz") &&
            (e.cover_art.available != 0 || (e.cover_art.data && e.cover_art.size > 0));
    };
    for (size_t i = 0; i < sorted_indices.size(); ++i) {
        const auto& e = entries->entries[sorted_indices[i]];
        std::string source = view_string(e.source_label);
        std::string source_display = source;
        bool has_cover = has_cover_art(e);
        if (has_cover) {
            source_display += " with cover art";
        }
        std::cout << "[" << (i + 1) << "] " << get_album_tag(&e, "ARTIST") << " - " << get_album_media_tag(&e)
                  << " (via " << source_display << ")\n";
    }
    std::cout << "[0] (Ignore all, not use these tags)\n";

    std::vector<size_t> choices;
    if (auto_mode) {
        if (!had_candidates_before_fallback) {
            choices = {0};
            std::cout << "\nAuto mode: no CDDB candidates; proceeding without selection.\n";
        } else {
            choices = {1};  // Always use the first entry (no merge).
            const auto& chosen = entries->entries[sorted_indices[0]];
            std::cout << "\nAuto mode: selected \"" << get_album_tag(&chosen, "ARTIST") << " - "
                      << get_album_media_tag(&chosen) << "\".\n";
        }
    } else {
        while (true) {
            std::cout << "\nSelect match [0-" << sorted_indices.size()
                      << "] (comma/space separated, default 1): ";
            std::string choice_line;
            if (!std::getline(std::cin, choice_line)) {
                choice_line.clear();
            }
            if (choice_line.empty()) {
                choices = {1};
                break;
            }

            std::string normalized = choice_line;
            for (char& ch : normalized) {
                if (ch == ',') ch = ' ';
            }
            std::istringstream iss(normalized);
            std::vector<int> nums;
            std::string token;
            bool parse_ok = true;
            while (iss >> token) {
                try {
                    nums.push_back(std::stoi(token));
                } catch (...) {
                    parse_ok = false;
                    break;
                }
            }
            if (!parse_ok || nums.empty()) {
                std::cerr << "Invalid selection. Example: 1 or 1,2 or 1 2\n";
                continue;
            }

            std::unordered_set<int> seen;
            std::vector<int> unique_nums;
            unique_nums.reserve(nums.size());
            for (int n : nums) {
                if (seen.insert(n).second) unique_nums.push_back(n);
            }

            bool has_zero = false;
            bool range_ok = true;
            for (int n : unique_nums) {
                if (n == 0) {
                    has_zero = true;
                    continue;
                }
                if (n < 0 || static_cast<size_t>(n) > sorted_indices.size()) {
                    range_ok = false;
                    break;
                }
            }
            if (!range_ok) {
                std::cerr << "Invalid selection range. Valid: 0-" << sorted_indices.size() << "\n";
                continue;
            }
            if (has_zero && unique_nums.size() > 1) {
                std::cerr << "Error: 0 must be selected alone.\n";
                continue;
            }

            choices.clear();
            choices.reserve(unique_nums.size());
            for (int n : unique_nums) choices.push_back(static_cast<size_t>(n));
            break;
        }
    }

    const bool ignored = (choices.size() == 1 && choices[0] == 0);
    result.entries = entries;
    result.ignored = ignored;
    if (ignored) {
        result.selected = nullptr;
        result.selected_entries.clear();
        return result;
    }

    std::vector<CdRipCddbEntry*> selected_entries;
    selected_entries.reserve(choices.size());
    std::unordered_set<size_t> seen_entry_indices;
    for (size_t n : choices) {
        if (n == 0) continue;
        if (n < 1 || n > sorted_indices.size()) continue;
        const size_t entry_index = sorted_indices[n - 1];
        if (seen_entry_indices.insert(entry_index).second) {
            selected_entries.push_back(&entries->entries[entry_index]);
        }
    }
    if (selected_entries.empty()) {
        selected_entries.push_back(&entries->entries[sorted_indices[0]]);
    }

    result.selected_entries = selected_entries;
    if (!auto_mode && selected_entries.size() > 1) {
        result.merged = merge_cddb_entries_for_toc(toc, selected_entries);
        if (result.merged && result.merged->count > 0 && result.merged->entries) {
            result.selected = &result.merged->entries[0];
        } else {
            if (result.merged) {
                cdrip_release_cddbentry_list(result.merged);
                result.merged = nullptr;
            }
            result.selected = selected_entries.front();
        }
    } else {
        result.selected = selected_entries.front();
    }
    return result;
}

std::string render_drive_list(const CdRipDetectedDriveList* candidates) {
    std::ostringstream oss;
    if (!candidates) return oss.str();
    for (size_t i = 0; i < candidates->count; ++i) {
        oss << "  [" << (i + 1) << "] " << view_string(candidates->drives[i].device)
            << " (media: " << (candidates->drives[i].has_media ? "present" : "none") << ")\n";
    }
    return oss.str();
}

std::optional<std::string> find_first_drive_with_media(
    const CdRipDetectedDriveList* candidates) {

    if (!candidates) return std::nullopt;
    for (size_t i = 0; i < candidates->count; ++i) {
        if (candidates->drives[i].has_media) {
            return view_string(candidates->drives[i].device);
        }
    }
    return std::nullopt;
}

bool lookup_drive_status(
    const CdRipDetectedDriveList* candidates,
    const std::string& device,
    bool& has_media) {

    if (!candidates) return false;
    const std::string target = canonicalize_device_path(device);
    for (size_t i = 0; i < candidates->count; ++i) {
        const std::string candidate = canonicalize_device_path(view_string(candidates->drives[i].device));
        if (candidate == target) {
            has_media = candidates->drives[i].has_media != 0;
            return true;
        }
    }
    return false;
}

std::optional<size_t> find_drive_index(
    const CdRipDetectedDriveList* candidates,
    const std::string& device) {

    if (!candidates) return std::nullopt;
    const std::string target = canonicalize_device_path(device);
    for (size_t i = 0; i < candidates->count; ++i) {
        const std::string candidate = canonicalize_device_path(view_string(candidates->drives[i].device));
        if (candidate == target) return i;
    }
    return std::nullopt;
}

std::optional<std::string> wait_for_media(
    const std::string& preferred_device,
    bool allow_any_device,
    const std::string& wait_message) {

    if (preferred_device.empty() && !allow_any_device) {
        return std::nullopt;
    }

    std::string device = preferred_device;
    std::string last_snapshot;
    bool message_printed = false;
    while (true) {
        CdRipDetectedDriveList* candidates = cdrip_detect_cd_drives();
        if (!candidates || candidates->count == 0) {
            cdrip_release_detecteddrive_list(candidates);
            std::cerr << "No CD drives detected. Specify device with -d <path>.\n";
            return std::nullopt;
        }

        if (!device.empty()) {
            bool has_media = false;
            bool found = lookup_drive_status(candidates, device, has_media);
            if (!found) {
                cdrip_release_detecteddrive_list(candidates);
                std::cerr << "Device " << device << " is not detected.\n";
                return std::nullopt;
            }
            if (has_media) {
                cdrip_release_detecteddrive_list(candidates);
                return device;
            }
        }

        if (device.empty() && allow_any_device) {
            auto first_with_media = find_first_drive_with_media(candidates);
            if (first_with_media) {
                std::string chosen = *first_with_media;
                cdrip_release_detecteddrive_list(candidates);
                return chosen;
            }
        }

        if (allow_any_device) {
            std::string snapshot = render_drive_list(candidates);
            if (snapshot != last_snapshot) {
                std::cout << "Detected CD drives:\n" << snapshot;
                last_snapshot = snapshot;
                message_printed = false;
            }
        }
        if (!message_printed) {
            std::cout << wait_message << "\n";
            message_printed = true;
        }
        cdrip_release_detecteddrive_list(candidates);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool wait_for_media_removal(
    const std::string& device,
    const std::string& wait_message) {

    bool message_printed = false;
    while (true) {
        CdRipDetectedDriveList* candidates = cdrip_detect_cd_drives();
        if (!candidates || candidates->count == 0) {
            cdrip_release_detecteddrive_list(candidates);
            std::cerr << "No CD drives detected while waiting for disc removal.\n";
            return false;
        }

        bool has_media = false;
        bool found = lookup_drive_status(candidates, device, has_media);
        if (!found) {
            cdrip_release_detecteddrive_list(candidates);
            std::cerr << "Device " << device << " is not detected while waiting for disc removal.\n";
            return false;
        }
        if (!has_media) {
            cdrip_release_detecteddrive_list(candidates);
            return true;
        }

        if (!message_printed) {
            std::cout << wait_message << "\n";
            message_printed = true;
        }
        cdrip_release_detecteddrive_list(candidates);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

enum class MediaWaitResult {
    Ready,
    Quit,
    Error,
};

MediaWaitResult wait_for_device_media_state(
    const std::string& device,
    bool expected_has_media,
    const std::string& wait_message,
    bool allow_quit) {

    bool message_printed = false;
    const bool allow_input = allow_quit && (::isatty(STDIN_FILENO) != 0);
    while (true) {
        CdRipDetectedDriveList* candidates = cdrip_detect_cd_drives();
        if (!candidates || candidates->count == 0) {
            cdrip_release_detecteddrive_list(candidates);
            std::cerr << "No CD drives detected while waiting for media.\n";
            return MediaWaitResult::Error;
        }

        bool has_media = false;
        bool found = lookup_drive_status(candidates, device, has_media);
        cdrip_release_detecteddrive_list(candidates);
        if (!found) {
            std::cerr << "Device " << device << " is not detected while waiting for media.\n";
            return MediaWaitResult::Error;
        }
        if (has_media == expected_has_media) {
            return MediaWaitResult::Ready;
        }

        if (!message_printed) {
            std::cout << wait_message << "\n";
            message_printed = true;
        }

        if (!allow_input) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int result = ::select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &tv);
        if (result < 0) {
            if (errno == EINTR) continue;
            std::cerr << "Failed while waiting for user input.\n";
            return MediaWaitResult::Error;
        }
        if (result > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                return MediaWaitResult::Error;
            }
            if (line == "q" || line == "Q") {
                return MediaWaitResult::Quit;
            }
        }
    }
}

}  // namespace

struct Options {
    std::optional<std::string> device; // empty = auto-detect
    std::optional<std::string> format;
    std::optional<int> compression_level;
    std::optional<int> max_width;
    std::optional<CdRipRipModes> rip_mode;
    std::optional<bool> repeat;
    std::optional<bool> sort;
    std::optional<std::string> filter_title;
    std::optional<bool> auto_mode;
    std::optional<bool> speed_fast;
    std::optional<std::string> discogs;
    std::string config_file;
    bool no_eject = false;
    bool no_aa = false;
    std::vector<std::string> update_paths;
};

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-d" || arg == "--device") && i + 1 < argc) {
            opts.device = argv[++i];
        } else if ((arg == "-f" || arg == "--format") && i + 1 < argc) {
            opts.format = argv[++i];
        } else if ((arg == "-c" || arg == "--compression") && i + 1 < argc) {
            opts.compression_level = std::stoi(argv[++i]);
        } else if ((arg == "-w" || arg == "--max-width") && i + 1 < argc) {
            int v = 0;
            try {
                v = std::stoi(argv[++i]);
            } catch (...) {
                std::cerr << "Error: -w/--max-width requires an integer\n";
                std::exit(1);
            }
            if (v <= 0) {
                std::cerr << "Error: -w/--max-width must be > 0\n";
                std::exit(1);
            }
            opts.max_width = v;
        } else if ((arg == "-m" || arg == "--mode") && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "fast") {
                opts.rip_mode = RIP_MODES_FAST;
            } else if (mode == "best") {
                opts.rip_mode = RIP_MODES_BEST;
            } else {
                opts.rip_mode = RIP_MODES_DEFAULT;
            }
        } else if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            opts.config_file = argv[++i];
        } else if (arg == "-r" || arg == "--repeat") {
            opts.repeat = true;
        } else if (arg == "-s" || arg == "--sort") {
            opts.sort = true;
        } else if ((arg == "-ft" || arg == "--filter-title") && i + 1 < argc) {
            opts.filter_title = argv[++i];
        } else if (arg == "-a" || arg == "--auto") {
            opts.auto_mode = true;
        } else if (arg == "-ss" || arg == "--speed-slow") {
            opts.speed_fast = false;
        } else if (arg == "-sf" || arg == "--speed-fast") {
            opts.speed_fast = true;
        } else if ((arg == "-dc" || arg == "--discogs") && i + 1 < argc) {
            opts.discogs = argv[++i];
        } else if (arg == "-na" || arg == "--no-aa") {
            opts.no_aa = true;
        } else if (arg == "-ne" || arg == "--no-eject") {
            opts.no_eject = true;
        } else if (arg == "-n") {
            std::cerr << "Warning: -n is deprecated; use -ne or --no-eject\n";
            opts.no_eject = true;
        } else if (arg == "-u" || arg == "--update") {
            if (i + 1 < argc) {
                opts.update_paths.push_back(argv[++i]);
            } else {
                std::cerr << "Error: -u/--update requires at least one path\n";
                std::exit(1);
            }
        } else if (arg == "-?" || arg == "-h" || arg == "--help") {
            std::cout << "Usage: cdrip [-d device] [-f format] [-m mode] [-c compression] [-w px] [--max-width px] [-s] [-ft regex] [-r] [-ne] [-a] [-ss|-sf] [-dc no|always|fallback] [-na] [-i config] [-u file|dir ...]\n";
            std::cout << "  -d  / --device: CD device path (default: auto-detect)\n";
            std::cout << "  -f  / --format: FLAC destination path format (default: \"{album/medium}/{tracknumber:02d}_{title:n}.flac\")\n";
            std::cout << "  -m  / --mode: Integrity check mode: \"best\" (full integrity checks, default), \"fast\" (disabled any checks)\n";
            std::cout << "  -c  / --compression: FLAC compression level (default: auto (best --> 5, fast --> 1))\n";
            std::cout << "  -w  / --max-width: Cover art max width in pixels (default: 512)\n";
            std::cout << "  -s  / --sort: Sort CDDB results by album name on the prompt\n";
            std::cout << "  -ft / --filter-title: Filter CDDB candidates by title using case-insensitive regex (UTF-8)\n";
            std::cout << "  -r  / --repeat: Prompt for next disc after finishing\n";
            std::cout << "  -ne / --no-eject: Keep disc in the drive after ripping finishes\n";
            std::cout << "  -a  / --auto: Enable fully automatic mode (without any prompts)\n";
            std::cout << "  -ss / --speed-slow: Request 1x drive read speed when ripping starts (default)\n";
            std::cout << "  -sf / --speed-fast: Request maximum drive read speed when ripping starts\n";
            std::cout << "  -dc / --discogs: Cover art preference for Discogs: no, always (default), fallback\n";
            std::cout << "  -na / --no-aa: Disable cover art ANSI/ASCII art output\n";
            std::cout << "  -i  / --input: cdrip config file path (default search: ./cdrip.conf --> ~/.cdrip.conf)\n";
            std::cout << "  -u  / --update <file|dir> [more ...]: Update existing FLAC tags from CDDB using embedded tags (other options ignored)\n";
            std::exit(0);
        }
    }
    return opts;
}

int run_update_mode(
    const std::vector<std::string>& target_paths,
    const CdRipCddbServerList* servers,
    bool sort,
    bool auto_mode,
    DiscogsMode discogs_mode,
    bool allow_aa,
    const GRegex* title_filter) {

    if (!servers || servers->count == 0) {
        std::cerr << "No CDDB servers configured.\n";
        return 1;
    }
    if (target_paths.empty()) {
        std::cerr << "Error: update mode requires at least one path.\n";
        return 1;
    }

    EntryListCache metadata_cache;
    size_t updated_total = 0;
    for (size_t pi = 0; pi < target_paths.size(); ++pi) {
        const std::string& target_path = target_paths[pi];
        std::cout << "\n=== Update target (" << (pi + 1) << "/" << target_paths.size() << "): " << target_path << " ===\n";

        const char* err = nullptr;
        CdRipTaggedTocList* list = cdrip_collect_cddb_queries_from_path(target_path.c_str(), &err);
        if (!list) {
            std::cerr << "Failed to collect targets.\n";
            return 1;
        }
        if (err) {
            std::cerr << view_string(err) << "\n";
            cdrip_release_error(err);
            err = nullptr;
        }
        if (list->count == 0) {
            std::cout << "No FLAC files found to update.\n";
            cdrip_release_taggedtoc_list(list);
            continue;
        }

        size_t updated = 0;
        for (size_t i = 0; i < list->count; ++i) {
            const auto& item = list->items[i];
            std::cout << "\n[" << (i + 1) << "/" << list->count << "] " << view_string(item.path) << "\n";
            if (!item.valid || !item.toc) {
                std::cout << "  Skipped: " << view_string(item.reason) << "\n";
                continue;
            }

            const std::string cache_key = build_metadata_cache_key(item.toc);
            auto selection = select_cddb_entry_for_toc(
                item.toc, servers, sort, view_string(item.path), auto_mode, /*allow_fallback=*/false, &metadata_cache, title_filter);
            if (!selection.entries || !selection.selected) {
                std::cout << "  Skipped: no metadata selected\n";
                if (selection.entries) cdrip_release_cddbentry_list(selection.entries);
                continue;
            }

            ensure_entry_ready_for_toc(selection.selected, item.toc);

            std::string cover_notice;
            CoverArtFetchSource cover_source{};
            if (!ensure_cover_art_merged(selection.selected, selection.selected_entries, item.toc, discogs_mode, cover_source, cover_notice, allow_aa)) {
                if (!cover_notice.empty()) {
                    std::cerr << "  Cover art fetch notice: " << cover_notice << "\n";
                }
            } else if (!cache_key.empty()) {
                metadata_cache[cache_key] = EntryListPtr(
                    clone_cddb_entry_list(selection.entries));
            }

            const char* update_err = nullptr;
            if (!cdrip_update_flac_with_cddb_entry(&item, selection.selected, &update_err)) {
                std::cout << "  Failed: " << view_string(update_err) << "\n";
                cdrip_release_error(update_err);
            } else {
                std::cout << "  Updated.\n";
                ++updated;
                ++updated_total;
            }
            cdrip_release_cddbentry_list(selection.entries);
            if (selection.merged) {
                cdrip_release_cddbentry_list(selection.merged);
            }
        }

        cdrip_release_taggedtoc_list(list);
        std::cout << "\nDone for target \"" << target_path << "\". Updated " << updated << " file(s).\n";
    }

    std::cout << "\nAll targets done. Updated " << updated_total << " file(s) in total.\n";
    return 0;
}

int main(int argc, char** argv) {
    std::cout << "\nScheme CD music/sound ripper [" << VERSION << "-" << COMMIT_ID << "]\n";
    std::cout << "Copyright (c) Kouji Matsui (@kekyo@mi.kekyo.net)\n";
    std::cout << "https://github.com/kekyo/scheme-cd-ripper\n";
    std::cout << "Licence: Under MIT.\n\n";

    Options cli_opts = parse_args(argc, argv);

    const char* config_err = nullptr;
    CdRipConfig* cfg_raw = cdrip_load_config(
        cli_opts.config_file.empty() ? nullptr : cli_opts.config_file.c_str(),
        &config_err);
    if (!cfg_raw) {
        std::cerr << (config_err ? view_string(config_err) : "Failed to load config") << "\n";
        cdrip_release_error(config_err);
        return 1;
    }
    cdrip_release_error(config_err);
    std::unique_ptr<CdRipConfig, decltype(&cdrip_release_config)> cfg(cfg_raw, &cdrip_release_config);

    std::string device = cli_opts.device.value_or(view_string(cfg->device));
    std::string format = cli_opts.format.value_or(view_string(cfg->format));
    int compression_level = cli_opts.compression_level.value_or(cfg->compression_level);
    const int max_width = cli_opts.max_width.value_or(cfg->max_width);
    CdRipRipModes rip_mode = cli_opts.rip_mode.value_or(cfg->mode);
    bool repeat = cli_opts.repeat.value_or(cfg->repeat);
    bool sort = cli_opts.sort.value_or(cfg->sort);
    bool auto_mode = cli_opts.auto_mode.value_or(cfg->auto_mode);
    bool eject_after = !cli_opts.no_eject;
    CdRipCddbServerList* servers_from_config = cfg->servers;

    std::string filter_title = trim_ws(cli_opts.filter_title.value_or(view_string(cfg->filter_title)));
    using RegexPtr = std::unique_ptr<GRegex, decltype(&g_regex_unref)>;
    RegexPtr title_filter(nullptr, &g_regex_unref);
    if (!filter_title.empty()) {
        GError* gerr = nullptr;
        GRegex* re = g_regex_new(
            filter_title.c_str(),
            static_cast<GRegexCompileFlags>(G_REGEX_CASELESS | G_REGEX_OPTIMIZE),
            static_cast<GRegexMatchFlags>(0),
            &gerr);
        if (!re) {
            const bool from_cli = cli_opts.filter_title.has_value();
            std::cerr << "Invalid " << (from_cli ? "-ft/--filter-title" : "cdrip.filter_title")
                      << " regex: " << (gerr && gerr->message ? gerr->message : "unknown error") << "\n";
            if (gerr) g_error_free(gerr);
            return 1;
        }
        title_filter.reset(re);
    }

    std::string aa_err;
    bool allow_aa = true;
    if (cfg->config_path && cfg->config_path[0]) {
        allow_aa = get_config_bool(cfg->config_path, "cdrip", "aa", /*default_value=*/true, aa_err);
        if (!aa_err.empty()) {
            std::cerr << "Failed to parse cdrip.aa from \"" << view_string(cfg->config_path) << "\": " << aa_err << "\n";
            return 1;
        }
    }
    if (cli_opts.no_aa) allow_aa = false;

    std::string speed_err;
    bool speed_fast = false;
    if (cfg->config_path && cfg->config_path[0]) {
        std::string speed_value = get_config_string(cfg->config_path, "cdrip", "speed", "slow", speed_err);
        if (!speed_err.empty()) {
            std::cerr << "Failed to parse cdrip.speed from \"" << view_string(cfg->config_path) << "\": " << speed_err << "\n";
            return 1;
        }
        std::transform(speed_value.begin(), speed_value.end(), speed_value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (speed_value == "fast") {
            speed_fast = true;
        } else if (speed_value == "slow") {
            speed_fast = false;
        } else {
            std::cerr << "Invalid cdrip.speed in \"" << view_string(cfg->config_path) << "\": " << speed_value << "\n";
            return 1;
        }
    }
    if (cli_opts.speed_fast.has_value()) speed_fast = *cli_opts.speed_fast;

    std::string discogs_err;
    std::string discogs_value = "always";
    if (cfg->config_path && cfg->config_path[0]) {
        discogs_value = get_config_string(cfg->config_path, "cdrip", "discogs", "always", discogs_err);
        if (!discogs_err.empty()) {
            std::cerr << "Failed to parse cdrip.discogs from \"" << view_string(cfg->config_path) << "\": " << discogs_err << "\n";
            return 1;
        }
    }
    if (cli_opts.discogs.has_value()) discogs_value = *cli_opts.discogs;

    DiscogsMode discogs_mode = DiscogsMode::Always;
    if (!parse_discogs_mode(discogs_value, discogs_mode)) {
        const bool from_cli = cli_opts.discogs.has_value();
        std::cerr << "Invalid " << (from_cli ? "-dc/--discogs" : "cdrip.discogs")
                  << " value: " << discogs_value << " (expected: no|always|fallback)\n";
        return 1;
    }
    if ((discogs_mode == DiscogsMode::Always || discogs_mode == DiscogsMode::Fallback) &&
        !servers_include_musicbrainz(servers_from_config)) {
        std::cerr << "Warning: Discogs is enabled (" << discogs_mode_label(discogs_mode)
                  << ") but MusicBrainz is not configured in [cddb].servers; disabling Discogs access.\n";
        discogs_mode = DiscogsMode::No;
    }

    cdrip_set_cover_art_max_width(max_width);

    if (!cli_opts.update_paths.empty()) {
        // Ignore other options when update mode is specified.
        return run_update_mode(cli_opts.update_paths, servers_from_config, cfg->sort, auto_mode, discogs_mode, allow_aa, title_filter.get());
    }

    const char* err = nullptr;
    if (auto_mode) {
        std::string wait_message;
        if (!device.empty()) {
            wait_message = "Waiting for media in " + device + " (auto mode)...";
        } else {
            wait_message = "Waiting for any drive with media (auto mode)...";
        }
        auto selected = wait_for_media(device, device.empty(), wait_message);
        if (!selected) return 1;
        device = *selected;
        std::cout << "\nUsing device: " << device << " (media: present)\n";
    } else {
        bool allow_single_drive_autoselect = device.empty();
        // auto-detect device if not provided, loop until media present on selection
        while (true) {
            CdRipDetectedDriveList* candidates = cdrip_detect_cd_drives();

            if (!candidates || candidates->count == 0) {
                std::cerr << "No CD drives detected. Specify device with -d <path>.\n";
                cdrip_release_detecteddrive_list(candidates);
                return 1;
            }

            // If user specified a device, check it first
	            if (!device.empty()) {
	                auto found_index = find_drive_index(candidates, device);
	                if (found_index) {
	                    const auto& found = candidates->drives[*found_index];
	                    if (found.has_media) {
	                        std::cout << "\nUsing device: " << view_string(found.device) << " (media: present)\n";
	                        cdrip_release_detecteddrive_list(candidates);
	                        break;
	                    }
	                    const std::string found_device = view_string(found.device);
	                    const std::string wait_message =
	                        "Media not present in " + found_device + ". Waiting for disc insertion...";
	                    cdrip_release_detecteddrive_list(candidates);
	                    auto ready = wait_for_media(device, false, wait_message);
	                    if (!ready) return 1;
	                    continue;
	                } else {
	                    std::cerr << "Device " << device << " is not detected. Specify device with -d <path>.\n";
	                    cdrip_release_detecteddrive_list(candidates);
	                    return 1;
                }
            }

            if (device.empty() && allow_single_drive_autoselect && candidates->count == 1) {
                const auto& only_drive = candidates->drives[0];
                device = view_string(only_drive.device);
	                if (only_drive.has_media) {
	                    std::cout << "\nUsing device: " << device << " (media: present)\n";
	                    cdrip_release_detecteddrive_list(candidates);
	                    break;
	                }

	                const std::string wait_message =
	                    "Media not present in " + device + ". Waiting for disc insertion...";
	                cdrip_release_detecteddrive_list(candidates);
	                auto ready = wait_for_media(device, false, wait_message);
	                if (!ready) return 1;
	                continue;
	            }

            std::cout << "Detected CD drives:\n";
            for (size_t i = 0; i < candidates->count; ++i) {
                std::cout << "  [" << (i + 1) << "] " << view_string(candidates->drives[i].device)
                          << " (media: " << (candidates->drives[i].has_media ? "present" : "none") << ")\n";
            }

            std::cout << "Select device [1-" << candidates->count << "] (default first with media, otherwise 1): ";
            std::string line;
            std::getline(std::cin, line);

            size_t choice = 0;
            if (!line.empty()) {
                try {
                    int parsed = std::stoi(line);
                    if (parsed >= 1 && static_cast<size_t>(parsed) <= candidates->count) {
                        choice = static_cast<size_t>(parsed - 1);
                    }
                } catch (...) {
                    std::cerr << "Invalid selection, picking default\n";
                }
            }

            if (choice == 0) {
                for (size_t i = 0; i < candidates->count; ++i) {
                    if (candidates->drives[i].has_media) {
                        choice = i;
                        break;
                    }
                }
            }

	            const auto& selected = candidates->drives[choice];
	            if (!selected.has_media) {
	                device = view_string(selected.device);
	                const std::string wait_message =
	                    "Media not present in " + device + ". Waiting for disc insertion...";
	                cdrip_release_detecteddrive_list(candidates);
	                auto ready = wait_for_media(device, false, wait_message);
	                if (!ready) return 1;
	                continue;
	            }

            device = view_string(selected.device);
            std::cout << "\nUsing device: " << device << " (media: present)\n";
            cdrip_release_detecteddrive_list(candidates);
            break;
        }
    }

    err = nullptr;
    CdRipSettings settings{format.c_str(), compression_level, rip_mode, speed_fast};
    auto drive = cdrip_open(device.c_str(), &settings, &err);
    if (!drive) {
        std::string err_msg = view_string(err);
        cdrip_release_error(err);
        err = nullptr;
        std::cerr << (err_msg.empty() ? "Could not open drive" : err_msg) << "\n";
        return 1;
    }

    std::cout << "\nOptions:\n";
    std::string config_source = cfg->config_path ? view_string(cfg->config_path) : std::string{"(defaults)"};
    std::cout << "  config      : \"" << config_source << "\"\n";
    std::cout << "  device      : \"" << device << "\"\n";
    std::cout << "  format      : \"" << format << "\"\n";
    CdRipRipModes effective_mode = (rip_mode == RIP_MODES_DEFAULT) ? RIP_MODES_BEST : rip_mode;
    int resolved_compression = compression_level >= 0
        ? compression_level
        : (effective_mode == RIP_MODES_FAST ? 1 : 5);
    std::cout << "  compression : " << resolved_compression;
    if (compression_level < 0) std::cout << " (auto)";
    std::cout << "\n";
    std::cout << "  mode        : ";
    switch (rip_mode) {
        case RIP_MODES_FAST:
            std::cout << "fast (disable any checks)";
            break;
        case RIP_MODES_BEST:
            std::cout << "best (full integrity checks)";
            break;
        default:
            std::cout << "default (best - full integrity checks)";
            break;
    }
    std::cout << "\n";
    std::cout << "  speed       : " << (speed_fast ? "fast (max)" : "slow (1x)") << "\n";
    std::cout << "  auto        : " << (auto_mode ? "enabled" : "disabled");
    std::cout << "\n\n";

    CdRipProgressCallback progress = &progress_cb;

    while (true) {
        if (!drive) {
            std::cerr << "Drive not available\n";
            return 1;
        }
        const char* toc_err = nullptr;
        CdRipDiscToc* toc = cdrip_build_disc_toc(drive, &toc_err);
        if (!toc || !toc->tracks || toc->tracks_count == 0) {
            if (toc_err) {
                std::cerr << view_string(toc_err) << "\n";
                cdrip_release_error(toc_err);
                toc_err = nullptr;
            } else {
                std::cerr << "No tracks detected\n";
            }
            cdrip_release_disctoc(toc);
            cdrip_close(drive, false, nullptr);
            return 1;
        }
        cdrip_release_error(toc_err);

        CdRipCddbServerList* servers = servers_from_config;

        auto selection = select_cddb_entry_for_toc(toc, servers, sort, std::string{}, auto_mode, /*allow_fallback=*/true, /*metadata_cache=*/nullptr, title_filter.get());
        const bool ignore_meta = (selection.selected == nullptr);
        if (!selection.entries) {
            std::cerr << "Failed to obtain CDDB entries\n";
            cdrip_release_cddbentry_list(selection.entries);
            cdrip_release_disctoc(toc);
            cdrip_close(drive, false, nullptr);
            return 1;
        }

        CdRipCddbEntry* fallback_meta = nullptr;
        CdRipCddbEntry* meta = selection.selected;
        if (!meta) {
            fallback_meta = make_fallback_entry(toc);
            // Clear source info to indicate "ignore all" selection.
            delete[] fallback_meta->source_label;
            delete[] fallback_meta->source_url;
            delete[] fallback_meta->fetched_at;
            fallback_meta->source_label = dup_cstr("");
            fallback_meta->source_url = dup_cstr("");
            fallback_meta->fetched_at = dup_cstr("");
            meta = fallback_meta;
        }

        ensure_entry_ready_for_toc(meta, toc, !ignore_meta);

        std::string cover_notice;
        CoverArtFetchSource cover_source{};
        if (ensure_cover_art_merged(meta, selection.selected_entries, toc, discogs_mode, cover_source, cover_notice, allow_aa)) {
            if (meta->cover_art.data && meta->cover_art.size > 0 && cover_source != CoverArtFetchSource::None) {
                std::cout << "\nCover art fetched from " << cover_art_source_label(cover_source) << ".\n";
            }
        } else if (!cover_notice.empty()) {
            std::cerr << "\nCover art fetch notice: " << cover_notice << "\n";
        }

        std::cout << "Start ripping...\n\n";

        std::vector<const CdRipTrackInfo*> audio_tracks;
        std::vector<double> track_secs;
        double total_album_sec = 0.0;
        for (size_t i = 0; i < toc->tracks_count; ++i) {
            const auto& track = toc->tracks[i];
            if (!track.is_audio) continue;
            // Audio CD uses 75 sectors (frames) per second; convert sector span to seconds.
            double sec = static_cast<double>(track.end - track.start + 1) / 75.0;
            audio_tracks.push_back(&track);
            track_secs.push_back(sec);
            total_album_sec += sec;
        }

        bool success = true;
        double completed_before = 0.0;
        int total_tracks = static_cast<int>(audio_tracks.size());
        double wall_start = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count() /
            1000.0;
        for (size_t idx = 0; idx < audio_tracks.size(); ++idx) {
            const auto* track = audio_tracks[idx];
            const char* rip_err = nullptr;
            if (!cdrip_rip_track(drive, track, meta, toc, progress, &rip_err, total_tracks, completed_before, total_album_sec, wall_start)) {
                success = false;
                if (rip_err) {
                    std::cerr << "Rip error: " << view_string(rip_err) << "\n";
                    cdrip_release_error(rip_err);
                }
                break;
            }
            cdrip_release_error(rip_err);
            completed_before += track_secs[idx];
        }

        if (success) {
            if (eject_after) {
                std::cout << "\nDone, will eject CD from the drive...\n";
            } else {
                std::cout << "\nDone, keeping CD in the drive (no-eject).\n";
            }
        } else {
            if (eject_after) {
                std::cout << "\nAborted with errors, will eject CD from the drive...\n";
            } else {
                std::cout << "\nAborted with errors, keeping CD in the drive (no-eject).\n";
            }
        }

        if (selection.entries) {
            cdrip_release_cddbentry_list(selection.entries);
        }
        if (selection.merged) {
            cdrip_release_cddbentry_list(selection.merged);
        }
        if (fallback_meta) {
            // make_fallback_entry allocates outside the list; clean up manually.
            if (fallback_meta->tracks) {
                for (size_t i = 0; i < fallback_meta->tracks_count; ++i) {
                    CdRipTrackTags* tt = &fallback_meta->tracks[i];
                    if (tt->tags) {
                        for (size_t k = 0; k < tt->tags_count; ++k) {
                            delete[] tt->tags[k].key;
                            delete[] tt->tags[k].value;
                        }
                        delete[] tt->tags;
                    }
                }
                delete[] fallback_meta->tracks;
            }
            delete[] fallback_meta->album_tags;
            if (fallback_meta->cover_art.data) {
                delete[] fallback_meta->cover_art.data;
            }
            delete[] fallback_meta->cover_art.mime_type;
            delete[] fallback_meta->cddb_discid;
            delete[] fallback_meta->source_label;
            delete[] fallback_meta->source_url;
            delete[] fallback_meta->fetched_at;
            delete fallback_meta;
        }
        cdrip_release_disctoc(toc);

        const char* close_err = nullptr;
        cdrip_close(drive, eject_after, &close_err);
        drive = nullptr;
        if (close_err) {
            std::cerr << view_string(close_err) << "\n";
            cdrip_release_error(close_err);
        }

        if (!repeat) return success ? 0 : 1;

        if (!auto_mode) {
            if (!eject_after) {
                const std::string removal_message =
                    "\nRemove disc from " + device + " (or type 'q' to quit)...";
                auto removed = wait_for_device_media_state(device, false, removal_message, true);
                if (removed == MediaWaitResult::Quit) return success ? 0 : 1;
                if (removed == MediaWaitResult::Error) return 1;
            }

            const std::string insert_message =
                "\nInsert next disc into " + device + " (or type 'q' to quit)...";
            auto inserted = wait_for_device_media_state(device, true, insert_message, true);
            if (inserted == MediaWaitResult::Quit) return success ? 0 : 1;
            if (inserted == MediaWaitResult::Error) return 1;
        } else {
            if (!eject_after) {
                std::string removal_message = "Waiting for disc removal from " + device + " (auto mode)...";
                if (!wait_for_media_removal(device, removal_message)) {
                    return 1;
                }
            }
            std::string wait_message = "Waiting for next disc in " + device + " (auto mode)...";
            auto next_device = wait_for_media(device, false, wait_message);
            if (!next_device) return 1;
            device = *next_device;
        }

        err = nullptr;
        drive = cdrip_open(device.c_str(), &settings, &err);
        if (!drive) {
            std::cerr << "Could not reopen drive " << device << ": " << view_string(err) << "\n";
            cdrip_release_error(err);
            err = nullptr;
            device.clear();
            return 1;
        }
        cdrip_release_error(err);
        err = nullptr;
    }
}
