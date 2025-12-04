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
#include <cstring>
#include <vector>
#include <map>

namespace {

std::string view_string(const char* s) {
    return s ? std::string{s} : std::string{};
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
    const int bar_width = 30;
    int filled = static_cast<int>(info->percent / 100.0 * bar_width);
    if (filled > bar_width) filled = bar_width;
    std::string bar(filled, '=');
    if (filled < bar_width) {
        bar.push_back('>');
        bar.append(bar_width - filled - 1, '-');
    }
    std::string track_name = view_string(info->track_name);
    if (track_name.empty()) track_name = view_string(info->title);
    std::cout << "\rTrack " << info->track_number << "/" << info->total_tracks
              << " [ETA: " << (show_eta ? fmt_time_fn(remaining_total) : "--:--") << "]: "
              << "\"" << track_name << "\" [" << bar << "]";
    std::cout.flush();
        if (info->percent >= 100.0) std::cout << "\n";
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

struct CddbSelection {
    CdRipCddbEntryList* entries{nullptr};
    CdRipCddbEntry* selected{nullptr};
    bool ignored{false};
};

CddbSelection select_cddb_entry_for_toc(
    const CdRipDiscToc* toc,
    const CdRipCddbServerList* servers,
    bool sort,
    const std::string& context_label = std::string{},
    bool auto_mode = false,
    bool allow_fallback = true,
    EntryListCache* metadata_cache = nullptr) {

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

    const bool had_initial_matches = entries && entries->count > 0;
    if ((!entries || entries->count == 0) && !allow_fallback) {
        std::cerr << "No CDDB matches found across configured servers; skipping metadata selection\n";
        result.entries = entries;
        result.selected = nullptr;
        result.ignored = true;
        return result;
    }
    if (!entries || entries->count == 0) {
        std::cerr << "No CDDB matches found across configured servers; using fallback metadata\n";
        cdrip_release_cddbentry_list(entries);
        entries = new CdRipCddbEntryList{};
        entries->count = 1;
        entries->entries = new CdRipCddbEntry[1]{};
        CdRipCddbEntry* fallback = make_fallback_entry(toc);
        if (fallback) {
            entries->entries[0] = *fallback;
            delete fallback;  // ownership transferred to entries->entries
        }
    }

    std::vector<size_t> sorted_indices(entries->count);
    for (size_t i = 0; i < entries->count; ++i) {
        sorted_indices[i] = i;
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
            std::string la = normalize_lower(get_album_tag(&l, "ALBUM"));
            std::string ra = normalize_lower(get_album_tag(&r, "ALBUM"));
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
        std::cout << "[" << (i + 1) << "] " << get_album_tag(&e, "ARTIST") << " - " << get_album_tag(&e, "ALBUM")
                  << " (via " << source_display << ")\n";
    }
    std::cout << "[0] (Ignore all, not use these tags)\n";

    size_t choice = 1;
    if (auto_mode) {
        if (!had_initial_matches) {
            choice = 0;
            std::cout << "\nAuto mode: no CDDB candidates; proceeding without selection.\n";
        } else {
            size_t preferred_index = sorted_indices.size();  // index in sorted_indices (0-based)
            for (size_t i = 0; i < sorted_indices.size(); ++i) {
                const auto& candidate = entries->entries[sorted_indices[i]];
                if (has_cover_art(candidate)) {
                    preferred_index = i;
                    break;
                }
            }
            if (preferred_index == sorted_indices.size()) {
                preferred_index = 0;
            }
            choice = preferred_index + 1;  // 1-based as with prompts
            const auto& chosen = entries->entries[sorted_indices[choice - 1]];
            std::cout << "\nAuto mode: selected \"" << get_album_tag(&chosen, "ARTIST") << " - "
                      << get_album_tag(&chosen, "ALBUM") << "\"";
            if (has_cover_art(chosen)) {
                std::cout << " (with cover art)";
            }
            std::cout << ".\n";
        }
    } else {
        std::cout << "\nSelect match [0-" << entries->count << "] (default 1): ";
        std::string choice_line;
        std::getline(std::cin, choice_line);
        if (!choice_line.empty()) {
            try {
                int parsed = std::stoi(choice_line);
                if (parsed == 0) choice = 0;
                else if (parsed >= 1 && static_cast<size_t>(parsed) <= entries->count) choice = static_cast<size_t>(parsed);
            } catch (...) {
                std::cerr << "Invalid selection, using first match\n";
            }
        }
    }

    CdRipCddbEntry* selected = (choice == 0)
        ? nullptr
        : &entries->entries[sorted_indices[choice - 1]];

    result.entries = entries;
    result.selected = selected;
    result.ignored = (choice == 0);
    if (choice == 0) {
        // Mark selection as ignored by leaving selected null.
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
    for (size_t i = 0; i < candidates->count; ++i) {
        if (view_string(candidates->drives[i].device) == device) {
            has_media = candidates->drives[i].has_media != 0;
            return true;
        }
    }
    return false;
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

        std::string snapshot = render_drive_list(candidates);
        if (snapshot != last_snapshot) {
            std::cout << "Detected CD drives:\n" << snapshot;
            last_snapshot = snapshot;
            message_printed = false;
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

    std::string last_snapshot;
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

        std::string snapshot = render_drive_list(candidates);
        if (snapshot != last_snapshot) {
            std::cout << "Detected CD drives:\n" << snapshot;
            last_snapshot = snapshot;
            message_printed = false;
        }
        if (!message_printed) {
            std::cout << wait_message << "\n";
            message_printed = true;
        }
        cdrip_release_detecteddrive_list(candidates);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}  // namespace

struct Options {
    std::optional<std::string> device; // empty = auto-detect
    std::optional<std::string> format;
    std::optional<int> compression_level;
    std::optional<CdRipRipModes> rip_mode;
    std::optional<bool> repeat;
    std::optional<bool> sort;
    std::optional<bool> auto_mode;
    std::string config_file;
    bool no_eject = false;
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
        } else if (arg == "-a" || arg == "--auto") {
            opts.auto_mode = true;
        } else if (arg == "-n" || arg == "--no-eject") {
            opts.no_eject = true;
        } else if (arg == "-u" || arg == "--update") {
            if (i + 1 < argc) {
                opts.update_paths.push_back(argv[++i]);
            } else {
                std::cerr << "Error: -u/--update requires at least one path\n";
                std::exit(1);
            }
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: cdrip [-d device] [-f format] [-m mode] [-c compression] [-s] [-r] [-n] [-a] [-i config] [-u file|dir ...]\n";
            std::cout << "  -d / --device: CD device path (default: auto-detect)\n";
            std::cout << "  -f / --format: FLAC destination path format (default: \"{album}/{tracknumber:02d}_{safetitle}.flac\")\n";
            std::cout << "  -m / --mode: Integrity check mode: \"best\" (full integrity checks, default), \"fast\" (disabled any checks)\n";
            std::cout << "  -c / --compression: FLAC compression level (default: auto (best --> 8, fast --> 1))\n";
            std::cout << "  -s / --sort: Sort CDDB results by album name on the prompt\n";
            std::cout << "  -r / --repeat: Prompt for next disc after finishing\n";
            std::cout << "  -n / --no-eject: Keep disc in the drive after ripping finishes\n";
            std::cout << "  -a / --auto: Enable fully automatic mode (without any prompts)\n";
            std::cout << "  -i / --input: cdrip config file path (default search: ./cdrip.conf --> ~/.cdrip.conf)\n";
            std::cout << "  -u / --update <file|dir> [more ...]: Update existing FLAC tags from CDDB using embedded tags (other options ignored)\n";
            std::exit(0);
        }
    }
    return opts;
}

int run_update_mode(
    const std::vector<std::string>& target_paths,
    const CdRipCddbServerList* servers,
    bool sort,
    bool auto_mode) {

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
                item.toc, servers, sort, view_string(item.path), auto_mode, /*allow_fallback=*/false, &metadata_cache);
            if (!selection.entries || !selection.selected) {
                std::cout << "  Skipped: no metadata selected\n";
                if (selection.entries) cdrip_release_cddbentry_list(selection.entries);
                continue;
            }

            ensure_entry_ready_for_toc(selection.selected, item.toc);

            const char* cover_err = nullptr;
            if (!cdrip_fetch_cover_art(selection.selected, item.toc, &cover_err)) {
                if (cover_err) {
                    std::cerr << "  Cover art fetch notice: " << view_string(cover_err) << "\n";
                    cdrip_release_error(cover_err);
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
    CdRipRipModes rip_mode = cli_opts.rip_mode.value_or(cfg->mode);
    bool repeat = cli_opts.repeat.value_or(cfg->repeat);
    bool sort = cli_opts.sort.value_or(cfg->sort);
    bool auto_mode = cli_opts.auto_mode.value_or(cfg->auto_mode);
    bool eject_after = !cli_opts.no_eject;
    CdRipCddbServerList* servers_from_config = cfg->servers;

    if (!cli_opts.update_paths.empty()) {
        // Ignore other options when update mode is specified.
        return run_update_mode(cli_opts.update_paths, servers_from_config, cfg->sort, auto_mode);
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
                size_t found_index = candidates->count;
                for (size_t i = 0; i < candidates->count; ++i) {
                    if (view_string(candidates->drives[i].device) == device) {
                        found_index = i;
                        break;
                    }
                }
                if (found_index < candidates->count) {
                    const auto& found = candidates->drives[found_index];
                    if (found.has_media) {
                        std::cout << "\nUsing device: " << view_string(found.device) << " (media: present)\n";
                        cdrip_release_detecteddrive_list(candidates);
                        break;
                    }
                    std::cout << "Media not present in " << view_string(found.device) << ". Insert disc and press Enter to re-scan.\n";
                    std::string dummy;
                    std::getline(std::cin, dummy);
                    device.clear();
                    cdrip_release_detecteddrive_list(candidates);
                    continue;
                } else {
                    std::cout << "Specified device not detected. Falling back to detected list.\n";
                    device.clear();
                }
            }

            if (device.empty() && allow_single_drive_autoselect && candidates->count == 1) {
                const auto& only_drive = candidates->drives[0];
                if (only_drive.has_media) {
                    device = view_string(only_drive.device);
                    std::cout << "\nUsing device: " << device << " (media: present)\n";
                    cdrip_release_detecteddrive_list(candidates);
                    break;
                }

                std::cout << "Only one CD drive detected (" << view_string(only_drive.device)
                          << ") but media is not present. Showing selection.\n";
                allow_single_drive_autoselect = false;
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
                std::cout << "Media not present in " << view_string(selected.device) << ". Insert disc and press Enter to re-scan.\n";
                std::string dummy;
                std::getline(std::cin, dummy);
                device.clear();
                allow_single_drive_autoselect = false;
                cdrip_release_detecteddrive_list(candidates);
                continue;
            }

            device = view_string(selected.device);
            std::cout << "\nUsing device: " << device << " (media: present)\n";
            cdrip_release_detecteddrive_list(candidates);
            break;
        }
    }

    err = nullptr;
    CdRipSettings settings{format.c_str(), compression_level, rip_mode};
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
        : (effective_mode == RIP_MODES_FAST ? 1 : 8);
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

        auto selection = select_cddb_entry_for_toc(toc, servers, sort, std::string{}, auto_mode);
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

        const char* cover_err = nullptr;
        if (cdrip_fetch_cover_art(meta, toc, &cover_err)) {
            if (meta->cover_art.data && meta->cover_art.size > 0) {
                std::cout << "\nCover art fetched from Cover Art Archive.\n";
            }
        } else if (cover_err) {
            std::cerr << "\nCover art fetch notice: " << view_string(cover_err) << "\n";
            cdrip_release_error(cover_err);
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
            std::cout << "\nInsert next disc into " << device << " and press Enter (or type 'q' to quit): ";
            std::string next;
            std::getline(std::cin, next);
            if (!next.empty() && (next == "q" || next == "Q")) {
                return success ? 0 : 1;
            }
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
