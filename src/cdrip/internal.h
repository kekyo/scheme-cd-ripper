#pragma once

// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <cdio/cdio.h>
#include <cdio/cd_types.h>
#include <ebur128.h>
#include <FLAC/metadata.h>

#include "cdrip/cdrip.h"

namespace cdrip::detail {
struct BackendDetectedDrive;
struct DriveBackend;
}

struct CdRip {
    const cdrip::detail::DriveBackend* backend{nullptr};
    void* drive{nullptr};
    void* reader{nullptr};
    CdRipRipModes mode{RIP_MODES_BEST};
    std::string device;
    std::string format;
    int compression_level{-1};
    bool speed_fast{false};
};

/* ------------------------------------------------------------------- */

namespace cdrip::detail {

struct BackendDetectedDrive {
    std::string device{};
    bool has_media{false};
};

// Thin indirection layer for all drive and reader interactions so tests can
// replace the live libcdio/cd-paranoia backend without changing public APIs.
struct DriveBackend {
    std::vector<BackendDetectedDrive> (*detect_drives)();
    bool (*open_drive)(
        const std::string& device,
        void*& out_drive,
        std::string& err);
    void (*close_drive)(void* drive);
    bool (*set_drive_speed)(
        void* drive,
        bool speed_fast,
        std::string& err);
    bool (*create_reader)(
        void* drive,
        CdRipRipModes mode,
        void*& out_reader,
        std::string& err);
    void (*destroy_reader)(void* reader);
    bool (*eject_drive)(
        const std::string& device,
        std::string& err);
    bool (*get_track_count)(
        void* drive,
        int& out_track_count,
        std::string& err);
    bool (*get_track_info)(
        void* drive,
        int track_number,
        CdRipTrackInfo& out_track,
        std::string& err);
    bool (*get_disc_last_sector)(
        void* drive,
        long& out_last_sector,
        std::string& err);
    bool (*seek_reader)(
        void* reader,
        long sector,
        std::string& err);
    bool (*read_sector)(
        void* reader,
        const int16_t*& out_buffer,
        std::string& err);
};

const DriveBackend& current_drive_backend();
void set_drive_backend_for_tests(
    const DriveBackend* backend);
void reset_drive_backend_for_tests();

// Helpers were previously static in the monolithic TU; keep internal linkage by
// providing inline definitions scoped to this header.
static inline const char* make_cstr_copy(const std::string& s) {
    auto* buf = new char[s.size() + 1];
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return buf;
}

static inline const char* make_cstr_copy(const char* s) {
    return make_cstr_copy(s ? std::string{s} : std::string{});
}

static inline std::string to_string_or_empty(const char* s) {
    return s ? std::string{s} : std::string{};
}

static inline std::string to_lower(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s) r.push_back(static_cast<char>(std::tolower(c)));
    return r;
}

static inline std::string to_upper(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s) r.push_back(static_cast<char>(std::toupper(c)));
    return r;
}

static inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

static inline void drop_format_only_tags(std::map<std::string, std::string>& tags) {
    tags.erase("MUSICBRAINZ_MEDIUMTITLE_RAW");
}

static inline bool is_replaygain_tag_key(
    const std::string& key_upper) {

    return key_upper == "REPLAYGAIN_TRACK_GAIN"
        || key_upper == "REPLAYGAIN_TRACK_PEAK"
        || key_upper == "REPLAYGAIN_ALBUM_GAIN"
        || key_upper == "REPLAYGAIN_ALBUM_PEAK"
        || key_upper == "REPLAYGAIN_REFERENCE_LOUDNESS";
}

static inline bool parse_int(const std::string& s, int& out) {
    try {
        out = std::stoi(s);
        return true;
    } catch (...) {
        return false;
    }
}

static inline bool parse_long(const std::string& s, long& out) {
    try {
        out = std::stol(s);
        return true;
    } catch (...) {
        return false;
    }
}

static inline void release_cstr(const char*& s) {
    delete[] s;
    s = nullptr;
}

static inline void set_error(const char** error, const std::string& message) {
    if (!error || *error) return;
    *error = make_cstr_copy(message);
}

static inline void clear_error(const char** error) {
    if (!error) return;
    cdrip_release_error(*error);
    *error = nullptr;
}

static inline bool has_activity_observer(
    const CdRipActivityObserver* observer) {

    return observer && observer->callback;
}

static inline void notify_activity(
    const CdRipActivityObserver* observer,
    void* state,
    const CdRipActivityInfo& info) {

    if (!has_activity_observer(observer)) return;
    observer->callback(&info, state, observer->user_data);
}

static inline bool has_diagnostic_observer(
    const CdRipDiagnosticObserver* observer) {

    return observer && observer->callback;
}

static inline void notify_diagnostic(
    const CdRipDiagnosticObserver* observer,
    void* state,
    const CdRipDiagnosticInfo& info) {

    if (!has_diagnostic_observer(observer)) return;
    observer->callback(&info, state, observer->user_data);
}

static inline CdRipTagKV make_kv(const std::string& key, const std::string& value) {
    CdRipTagKV kv{};
    kv.key = make_cstr_copy(to_upper(key));
    kv.value = make_cstr_copy(value);
    return kv;
}

static inline std::string find_tag(
    const CdRipTagKV* tags,
    size_t count,
    const std::string& key_upper) {

    for (size_t i = 0; i < count; ++i) {
        if (to_upper(to_string_or_empty(tags[i].key)) == key_upper) {
            return to_string_or_empty(tags[i].value);
        }
    }
    return std::string{};
}

static inline std::string album_tag(const CdRipCddbEntry* entry, const std::string& key) {
    if (!entry || !entry->album_tags) return {};
    return find_tag(entry->album_tags, entry->album_tags_count, to_upper(key));
}

static inline std::string track_tag(
    const CdRipCddbEntry* entry,
    size_t track_index_zero_based,
    const std::string& key) {

    if (!entry || !entry->tracks ||
        track_index_zero_based >= entry->tracks_count) return {};
    const auto& tt = entry->tracks[track_index_zero_based];
    return find_tag(tt.tags, tt.tags_count, to_upper(key));
}

std::vector<std::string> extract_album_title_candidates(
    const std::vector<const CdRipCddbEntry*>& entries);

/**
 * Build MusicBrainz entries from a release JSON payload using the same
 * media-selection path as runtime code.
 * @param toc Disc TOC used for medium matching.
 * @param request_url Source URL used for diagnostics.
 * @param release_json Raw MusicBrainz release JSON.
 * @param strict_title_recrawl_match True to enable the stricter recrawl-only filter.
 * @param track_length_tolerance_percent Allowed per-track length drift in percent.
 * @param log_rejections True to emit rejection diagnostics.
 * @param results Output entries.
 * @param err Output error text on failure.
 * @return True if JSON parsing succeeded; false on parse/shape failure.
 */
bool build_musicbrainz_entries_from_release_json(
    const CdRipDiscToc* toc,
    const std::string& request_url,
    const std::string& release_json,
    bool strict_title_recrawl_match,
    int track_length_tolerance_percent,
    bool log_rejections,
    std::vector<CdRipCddbEntry>& results,
    std::string& err);

/**
 * Release entries allocated by build_musicbrainz_entries_from_release_json.
 * @param entries Entry vector to clear.
 */
void release_cddb_entries(
    std::vector<CdRipCddbEntry>& entries);

static inline FLAC__StreamMetadata* build_vorbis_comments(
    const std::map<std::string, std::string>& tags) {

    FLAC__StreamMetadata* meta = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    if (!meta) return nullptr;
    for (const auto& [key, value] : tags) {
        FLAC__StreamMetadata_VorbisComment_Entry entry;
        if (FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, key.c_str(), value.c_str())) {
            FLAC__metadata_object_vorbiscomment_append_comment(meta, entry, false);
        }
    }
    return meta;
}

static inline bool has_cover_art_data(const CdRipCoverArt& art) {
    return art.data != nullptr && art.size > 0;
}

static inline FLAC__StreamMetadata* build_picture_block(
    const CdRipCoverArt& art) {

    if (!has_cover_art_data(art)) return nullptr;
    if (art.size > std::numeric_limits<FLAC__uint32>::max()) {
        return nullptr;
    }
    // FLAC metadata block size is limited to ~16MB; skip if image is too large.
    constexpr FLAC__uint32 kMaxPictureBytes = 16 * 1024 * 1024 - 1;
    if (art.size > kMaxPictureBytes) {
        return nullptr;
    }

    FLAC__StreamMetadata* pic =
        FLAC__metadata_object_new(FLAC__METADATA_TYPE_PICTURE);
    if (!pic) return nullptr;

    const char* mime = (art.mime_type && art.mime_type[0] != '\0')
        ? art.mime_type
        : "image/jpeg";
    const FLAC__uint32 length = static_cast<FLAC__uint32>(art.size);

    pic->data.picture.type = art.is_front
        ? FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER
        : FLAC__STREAM_METADATA_PICTURE_TYPE_OTHER;

    auto read_be32 = [](const uint8_t* p) -> FLAC__uint32 {
        return (static_cast<FLAC__uint32>(p[0]) << 24) |
               (static_cast<FLAC__uint32>(p[1]) << 16) |
               (static_cast<FLAC__uint32>(p[2]) << 8) |
               static_cast<FLAC__uint32>(p[3]);
    };
    auto try_parse_png_ihdr = [&](FLAC__uint32& w, FLAC__uint32& h, FLAC__uint32& depth) -> bool {
        if (!art.data || art.size < 33) return false;
        const uint8_t* d = reinterpret_cast<const uint8_t*>(art.data);
        static constexpr uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (std::memcmp(d, kPngSig, 8) != 0) return false;
        const FLAC__uint32 ihdr_len = read_be32(d + 8);
        if (ihdr_len < 13) return false;
        if (std::memcmp(d + 12, "IHDR", 4) != 0) return false;
        const FLAC__uint32 pw = read_be32(d + 16);
        const FLAC__uint32 ph = read_be32(d + 20);
        if (pw == 0 || ph == 0) return false;
        const uint8_t bit_depth = d[24];
        const uint8_t color_type = d[25];
        FLAC__uint32 channels = 0;
        switch (color_type) {
            case 0: channels = 1; break;  // grayscale
            case 2: channels = 3; break;  // rgb
            case 3: channels = 1; break;  // palette
            case 4: channels = 2; break;  // gray+alpha
            case 6: channels = 4; break;  // rgba
            default: return false;
        }
        if (bit_depth == 0) return false;
        w = pw;
        h = ph;
        depth = channels * static_cast<FLAC__uint32>(bit_depth);
        return true;
    };

    // Dimensions: prefer parsing PNG IHDR when available; otherwise leave unspecified.
    pic->data.picture.width = 0;
    pic->data.picture.height = 0;
    pic->data.picture.depth = 0;
    pic->data.picture.colors = 0;
    FLAC__uint32 w = 0, h = 0, depth = 0;
    if (try_parse_png_ihdr(w, h, depth)) {
        pic->data.picture.width = w;
        pic->data.picture.height = h;
        pic->data.picture.depth = depth;
    }

    if (!FLAC__metadata_object_picture_set_mime_type(
            pic,
            const_cast<char*>(mime),
            /*copy=*/true)) {
        FLAC__metadata_object_delete(pic);
        return nullptr;
    }

    if (!FLAC__metadata_object_picture_set_description(
            pic,
            reinterpret_cast<FLAC__byte*>(const_cast<char*>("")),
            /*copy=*/true)) {
        FLAC__metadata_object_delete(pic);
        return nullptr;
    }

    if (!FLAC__metadata_object_picture_set_data(
            pic,
            const_cast<FLAC__byte*>(
                reinterpret_cast<const FLAC__byte*>(art.data)),
            length,
            /*copy=*/true)) {
        FLAC__metadata_object_delete(pic);
        return nullptr;
    }

    // Validate the final picture block to catch malformed headers early.
    const char* violation = nullptr;
    if (!FLAC__format_picture_is_legal(&pic->data.picture, &violation)) {
        FLAC__metadata_object_delete(pic);
        return nullptr;
    }
    return pic;
}

static inline void apply_tag_kvs(
    std::map<std::string, std::string>& tags,
    const CdRipTagKV* kvs,
    size_t count) {

    for (size_t i = 0; i < count; ++i) {
        std::string key = to_upper(to_string_or_empty(kvs[i].key));
        std::string val = to_string_or_empty(kvs[i].value);
        if (!key.empty() && !val.empty()) {
            tags[key] = val;
        }
    }
}

static inline void prune_empty_tags(
    std::map<std::string, std::string>& tags) {

    for (auto it = tags.begin(); it != tags.end();) {
        if (it->second.empty()) {
            it = tags.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace cdrip::detail

// Helpers shared across translation units.
namespace cdrip::detail {
bool compute_musicbrainz_discid(
    const CdRipDiscToc* toc,
    std::string& out_discid,
    long& out_leadout);

struct ReplayGainScanResult {
    double loudness_lufs{0.0};
    double peak{0.0};
    bool loudness_ok{false};
    bool peak_ok{false};
};

struct RipTrackWriteOptions {
    const char* output_path{nullptr};
    const char* display_path{nullptr};
    ebur128_state* track_replaygain_state{nullptr};
    ebur128_state* album_replaygain_state{nullptr};
};

static inline std::string build_cddb_offsets_tag(
    const CdRipDiscToc* toc) {

    if (!toc || !toc->tracks || toc->tracks_count == 0) return {};
    std::ostringstream oss;
    for (size_t i = 0; i < toc->tracks_count; ++i) {
        if (i > 0) oss << ",";
        oss << toc->tracks[i].start;
    }
    return oss.str();
}

static inline void append_requery_seed_tags(
    std::map<std::string, std::string>& tags,
    const CdRipDiscToc* toc,
    const std::string& cddb_discid,
    int track_number,
    int track_total) {

    if (track_number > 0) {
        tags["TRACKNUMBER"] = std::to_string(track_number);
    }
    if (track_total > 0) {
        tags["TRACKTOTAL"] = std::to_string(track_total);
    }
    if (!cddb_discid.empty()) {
        tags["CDDB_DISCID"] = cddb_discid;
    }

    const std::string offsets = build_cddb_offsets_tag(toc);
    if (!offsets.empty()) {
        tags["CDDB_OFFSETS"] = offsets;
    }
    if (toc && toc->length_seconds > 0) {
        tags["CDDB_TOTAL_SECONDS"] = std::to_string(toc->length_seconds);
    }
    if (!toc) return;

    const std::string mb_discid = to_string_or_empty(toc->mb_discid);
    if (!mb_discid.empty()) {
        tags["MUSICBRAINZ_DISCID"] = mb_discid;
        const long mb_leadout = (toc->leadout_sector > 0)
            ? (toc->leadout_sector + 150)
            : 0;
        if (mb_leadout > 0) {
            tags["MUSICBRAINZ_LEADOUT"] = std::to_string(mb_leadout);
        }
    }

    const std::string mb_release = to_string_or_empty(toc->mb_release_id);
    if (!mb_release.empty() && tags.find("MUSICBRAINZ_RELEASE") == tags.end()) {
        tags["MUSICBRAINZ_RELEASE"] = mb_release;
    }
    const std::string mb_medium = to_string_or_empty(toc->mb_medium_id);
    if (!mb_medium.empty() && tags.find("MUSICBRAINZ_MEDIUM") == tags.end()) {
        tags["MUSICBRAINZ_MEDIUM"] = mb_medium;
    }
}

std::map<std::string, std::string> build_track_vorbis_tags(
    const CdRipTrackInfo* track,
    const CdRipCddbEntry* meta,
    const CdRipDiscToc* toc,
    int total_tracks,
    std::string& title_out,
    std::string& track_name_out,
    std::string& safe_title_out);

bool resolve_track_output_path(
    const std::string& format,
    const std::map<std::string, std::string>& tags,
    std::string& out_path,
    std::string& err);

bool publish_local_file_to_destination(
    const std::string& local_path,
    const std::string& destination_path,
    std::string& err);

bool finalize_replaygain_scan(
    ebur128_state* state,
    ReplayGainScanResult& out,
    std::string& err);

std::map<std::string, std::string> build_replaygain_tags(
    const ReplayGainScanResult& track,
    const ReplayGainScanResult& album);

bool update_flac_tags(
    const std::string& path,
    const CdRipDiscToc* toc,
    int track_number,
    const CdRipCddbEntry* entry,
    const std::map<std::string, std::string>& extra_tags,
    bool preserve_replaygain_tags,
    std::string& err);

bool rip_track_with_options(
    CdRip* rip,
    const CdRipTrackInfo* track,
    const CdRipCddbEntry* meta,
    const CdRipDiscToc* toc,
    CdRipProgressCallback progress,
    int total_tracks,
    double completed_before_sec,
    double total_album_sec,
    double wall_start_sec,
    const RipTrackWriteOptions* options,
    ReplayGainScanResult* replaygain_result,
    std::string& err);
}
