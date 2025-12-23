// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <cdio/cdio.h>
#include <cdio/cd_types.h>
#include <FLAC++/encoder.h>
#include <gio/gio.h>
#include <glib.h>

#include "internal.h"
#include "format_value.h"

using namespace cdrip::detail;

/* ------------------------------------------------------------------- */
/* Use static linkage for file local definitions */

constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;
constexpr int kSampleRate = 44100;
constexpr int kSamplesPerSector = CDIO_CD_FRAMESIZE_RAW / (kChannels * sizeof(int16_t));

struct GioSink {
    GFile* file{nullptr};
    GFile* tmp_file{nullptr};
    GFile* temp_file{nullptr};
    std::string tmp_uri;
    std::string final_uri;
    std::string temp_path;
};

static const std::string reserved = "\\:?\"<>|*";

static std::string sanitize_component(
    const std::string& input) {

    std::string result;
    result.reserve(input.size());
    for (unsigned char uch : input) {
        char ch = static_cast<char>(uch);
        if (std::iscntrl(uch) || reserved.find(ch) != std::string::npos || ch == '/') {
            result.push_back('_');
        } else {
            result.push_back(ch);
        }
    }
    if (result.empty()) result = "track";
    return result;
}

static std::string sanitize_path_components(
    const std::string& path,
    bool leading_slash) {

    std::ostringstream oss;
    if (leading_slash) {
        oss << "/";
    }

    std::string part;
    std::istringstream iss(path);
    bool first = true;
    while (std::getline(iss, part, '/')) {
        if (!first) {
            oss << "/";
        }
        oss << sanitize_component(part);
        first = false;
    }
    return oss.str();
}

static std::string sanitize_path(
    const std::string& path) {

    const auto scheme_pos = path.find("://");
    if (scheme_pos != std::string::npos) {
        // Preserve URI scheme/authority; sanitize only the path portion.
        const std::string scheme = path.substr(0, scheme_pos);
        const std::string rest = path.substr(scheme_pos + 3);  // skip "://"
        const auto authority_end = rest.find('/');
        if (authority_end == std::string::npos) {
            // No path part; nothing to sanitize.
            return scheme + "://" + rest;
        }
        const std::string authority = rest.substr(0, authority_end);
        const std::string uri_path = rest.substr(authority_end + 1);
        return scheme + "://" + authority +
            sanitize_path_components(uri_path, /*leading_slash=*/true);
    }

    const bool leading_slash = !path.empty() && path.front() == '/';
    const std::string path_no_leading = leading_slash ? path.substr(1) : path;
    return sanitize_path_components(path_no_leading, leading_slash);
}

static bool is_uri(const std::string& path) {
    return path.find("://") != std::string::npos;
}

static std::string truncate_on_newline(
    const std::string& s) {

    const size_t pos_ctrl = s.find_first_of("\r\n");
    const size_t pos_lit_n = s.find("\\n");
    const size_t pos_lit_r = s.find("\\r");

    size_t pos = std::string::npos;

    if (pos_ctrl != std::string::npos) pos = pos_ctrl;
    if (pos_lit_n != std::string::npos) pos = (pos == std::string::npos) ? pos_lit_n : std::min(pos, pos_lit_n);
    if (pos_lit_r != std::string::npos) pos = (pos == std::string::npos) ? pos_lit_r : std::min(pos, pos_lit_r);
    if (pos == std::string::npos) return s;
    return s.substr(0, pos);
}

static bool is_numeric_format_key(const std::string& key_upper) {
    return key_upper == "TRACKNUMBER"
        || key_upper == "TRACKTOTAL"
        || key_upper == "DISCNUMBER"
        || key_upper == "DISCTOTAL"
        || key_upper == "CDDB_TOTAL_SECONDS"
        || key_upper == "MUSICBRAINZ_LEADOUT";
}

static bool parse_int_strict(const std::string& s, int& out) {
    const std::string trimmed = trim(s);
    if (trimmed.empty()) return false;
    size_t idx = 0;
    try {
        int value = std::stoi(trimmed, &idx);
        if (idx != trimmed.size()) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

static FormatTagMap build_format_tags(
    const std::map<std::string, std::string>& path_tags) {

    FormatTagMap format_tags;
    for (const auto& [key, value] : path_tags) {
        const std::string key_upper = to_upper(key);
        if (is_numeric_format_key(key_upper)) {
            int numeric = 0;
            if (parse_int_strict(value, numeric)) {
                format_tags[key_upper] = std::make_unique<NumericValue>(numeric, value);
                continue;
            }
        }
        format_tags[key_upper] = std::make_unique<StringValue>(value);
    }
    return format_tags;
}

static std::string format_filename(
    const std::string& fmt,
    const FormatTagMap& tags) {

    std::string out;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '{') {
            size_t end = fmt.find('}', i);
            if (end != std::string::npos) {
                std::string token = fmt.substr(i + 1, end - i - 1);
                const FormatExpression expr = parse_format_expression(token);
                out += format_token_expression(expr, tags);
                i = end;
                continue;
            }
        }
        out.push_back(fmt[i]);
    }
    if (out.size() < 5 || out.substr(out.size() - 5) != ".flac") {
        out += ".flac";
    }
    return sanitize_path(out);
}

static const std::string trailing_trim = ".,;|~/\\^";
static const std::string replace_chars = ".:;|/\\^";

/* ------------------------------------------------------------------- */
/* Exported API functions */

extern "C" {

int cdrip_rip_track(
    CdRip* rip,
    const CdRipTrackInfo* track,
    const CdRipCddbEntry* meta,
    const CdRipDiscToc* toc,
    CdRipProgressCallback progress,
    const char** error,
    int total_tracks,
    double completed_before_sec,
    double total_album_sec,
    double wall_start_sec) {

    clear_error(error);
    if (!rip) return 0;
    if (!track || !meta || !toc) {
        set_error(error, "Invalid arguments to cdrip_rip_track");
        return 0;
    }
    if (!track->is_audio) {
        std::cout << "Skipping data track " << track->number << "\n";
        return 1;
    }

    const long sectors = track->end - track->start + 1;
    if (sectors <= 0) {
        set_error(error,
            "Track "
            + std::to_string(track->number)
            + " has invalid length");
        return 0;
    }

    const std::string meta_title = track_tag(meta, static_cast<size_t>(track->number - 1), "TITLE");
    const std::string title = !meta_title.empty()
        ? meta_title
        : ("Track " + std::to_string(track->number));
    const std::string meta_artist = album_tag(meta, "ARTIST");
    const std::string meta_album = album_tag(meta, "ALBUM");
    const std::string meta_genre = album_tag(meta, "GENRE");
    const std::string meta_year = album_tag(meta, "DATE");
    const std::string meta_discid = to_string_or_empty(meta->cddb_discid);
    const std::string meta_source_label = to_string_or_empty(meta->source_label);
    const std::string meta_source_url = to_string_or_empty(meta->source_url);
    const std::string meta_fetched_at = to_string_or_empty(meta->fetched_at);
    const bool ignore_source = meta_source_label.empty() && meta_source_url.empty();
    std::string fetched_for_tag;
    if (!ignore_source && meta_fetched_at.empty()) {
        char* ts = cdrip_current_timestamp_iso();
        fetched_for_tag = to_string_or_empty(ts);
        cdrip_release_timestamp(ts);
    } else {
        fetched_for_tag = meta_fetched_at;
    }
    const std::string cddb_discid = !meta_discid.empty()
        ? meta_discid
        : to_string_or_empty(toc->cddb_discid);
    std::string cddb_offsets;
    if (toc->tracks && toc->tracks_count > 0) {
        std::ostringstream oss;
        for (size_t i = 0; i < toc->tracks_count; ++i) {
            if (i > 0) oss << ",";
            oss << toc->tracks[i].start;
        }
        cddb_offsets = oss.str();
    }
    const std::string cddb_total_seconds = (toc->length_seconds > 0)
        ? std::to_string(toc->length_seconds)
        : std::string{};

    std::map<std::string, std::string> tags = {
        {"TITLE", title},
        {"ARTIST", meta_artist},
        {"ALBUM", meta_album},
        {"GENRE", meta_genre},
        {"DATE", meta_year},
        {"TRACKNUMBER", std::to_string(track->number)},
        {"TRACKTOTAL", std::to_string(total_tracks)},
        {"CDDB_DISCID", cddb_discid},
        {"CDDB_OFFSETS", cddb_offsets},
        {"CDDB_TOTAL_SECONDS", cddb_total_seconds},
    };
    if (!ignore_source) {
        tags["CDDB"] = meta_source_label;
        tags["CDDB_DATE"] = fetched_for_tag;
        // CDDB_URL intentionally skipped.
    }

    auto apply_tags = [&](const CdRipTagKV* kvs, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            std::string key = to_upper(to_string_or_empty(kvs[i].key));
            std::string val = to_string_or_empty(kvs[i].value);
            if (!key.empty() && !val.empty()) {
                if (key == "MUSICBRAINZ_MEDIUMTITLE") continue;
                tags[key] = val;
            }
        }
    };

    if (meta->album_tags && meta->album_tags_count > 0) {
        apply_tags(meta->album_tags, meta->album_tags_count);
    }
    if (meta->tracks &&
        static_cast<size_t>(track->number - 1) < meta->tracks_count) {
        const auto& tt = meta->tracks[static_cast<size_t>(track->number - 1)];
        if (tt.tags && tt.tags_count > 0) {
            apply_tags(tt.tags, tt.tags_count);
        }
    }

    if (ignore_source) {
        const std::string mb_discid = to_string_or_empty(toc->mb_discid);
        if (!mb_discid.empty()) {
            tags["MUSICBRAINZ_DISCID"] = mb_discid;
            long mb_leadout = toc->leadout_sector > 0
                ? toc->leadout_sector + 150
                : 0;
            if (mb_leadout > 0) {
                tags["MUSICBRAINZ_LEADOUT"] = std::to_string(mb_leadout);
            }
        }
    }

    auto prune_empty = [](std::map<std::string, std::string>& m) {
        for (auto it = m.begin(); it != m.end();) {
            if (it->second.empty()) it = m.erase(it);
            else ++it;
        }
    };
    prune_empty(tags);

    std::map<std::string, std::string> path_tags = tags;
    for (auto& kv : path_tags) {
        kv.second = truncate_on_newline(kv.second);
    }
    const std::string track_name = truncate_on_newline(path_tags["TITLE"]);
    path_tags["TITLE"] = track_name;
    std::string safe_title = track_name;
    while (!safe_title.empty() &&
        trailing_trim.find(safe_title.back()) != std::string::npos) {
        safe_title.pop_back();
    }
    for (char& ch : safe_title) {
        if (replace_chars.find(ch) != std::string::npos) {
            ch = '_';
        }
    }
    path_tags["SAFETITLE"] = safe_title;

    auto build_album_media = [&]() -> std::string {
        const std::string album = trim(truncate_on_newline(path_tags["ALBUM"]));
        int disctotal = 0;
        parse_int(trim(truncate_on_newline(path_tags["DISCTOTAL"])), disctotal);
        if (disctotal <= 1) return album;

        const std::string medium_title = trim(truncate_on_newline(album_tag(meta, "MUSICBRAINZ_MEDIUMTITLE")));
        if (!medium_title.empty()) {
            if (album.empty()) return medium_title;
            return album + " " + medium_title;
        }

        const std::string discnumber = trim(truncate_on_newline(path_tags["DISCNUMBER"]));
        if (discnumber.empty()) return album;
        if (album.empty()) return "CD" + discnumber;
        return album + " CD" + discnumber;
    };
    path_tags["ALBUMMEDIA"] = build_album_media();

    FormatTagMap format_tags = build_format_tags(path_tags);
    const std::string fmt = !rip->format.empty() ? rip->format : "";
    const std::string outfile = format_filename(fmt, format_tags);
    const bool uri_output = is_uri(outfile);

    GioSink sink;
    sink.final_uri = outfile;
    sink.tmp_uri = outfile + ".tmp";

    GError* gerr = nullptr;
    sink.file = uri_output
        ? g_file_new_for_uri(sink.final_uri.c_str())
        : g_file_new_for_path(outfile.c_str());
    sink.tmp_file = uri_output
        ? g_file_new_for_uri(sink.tmp_uri.c_str())
        : g_file_new_for_path(sink.tmp_uri.c_str());

    // Ensure parent directories exist (for local paths) or via GIO for URIs
    GFile* parent = g_file_get_parent(sink.tmp_file);
    if (parent) {
        if (!g_file_make_directory_with_parents(parent, nullptr, &gerr)) {
            if (gerr && gerr->code != G_IO_ERROR_EXISTS) {
                set_error(error,
                    std::string("Failed to create directories for ")
                    + sink.tmp_uri
                    + ": "
                    + (gerr ? gerr->message : "unknown"));
                g_clear_error(&gerr);
                g_object_unref(parent);
                g_object_unref(sink.file);
                g_object_unref(sink.tmp_file);
                return 0;
            }
            g_clear_error(&gerr);
        }
        g_object_unref(parent);
    }

    gchar* temp_path_c = nullptr;
    const int temp_fd = g_file_open_tmp(
        "cdripXXXXXX.flac", &temp_path_c, &gerr);
    if (temp_fd == -1) {
        set_error(error,
            std::string("Failed to create temporary file: ") +
            (gerr ? gerr->message : "unknown"));
        g_clear_error(&gerr);
        g_object_unref(sink.file);
        g_object_unref(sink.tmp_file);
        return 0;
    }
    close(temp_fd);
    sink.temp_path = temp_path_c ? temp_path_c : "";
    g_free(temp_path_c);
    sink.temp_file = g_file_new_for_path(sink.temp_path.c_str());
    if (!sink.temp_file) {
        set_error(error, "Failed to reference temporary file");
        g_object_unref(sink.file);
        g_object_unref(sink.tmp_file);
        return 0;
    }

    int compression_level = rip->compression_level;
    if (compression_level < 0) {
        compression_level = (rip->mode == RIP_MODES_FAST) ? 1 : 5;
    }

    // Request rip speed (1 => 1x, 0 => rip maximum).
    // Ignore errors; not all drives support it.
    cdda_speed_set(rip->drive, rip->speed_fast ? 0 : 1);

    FLAC::Encoder::File encoder;
    encoder.set_verify(false);
    encoder.set_compression_level(compression_level);
    encoder.set_channels(kChannels);
    encoder.set_bits_per_sample(kBitsPerSample);
    encoder.set_sample_rate(kSampleRate);
    encoder.set_total_samples_estimate(
        static_cast<uint64_t>(sectors) * kSamplesPerSector);

    // Prepare Vorbis and cover art metadata; attach to encoder
    FLAC__StreamMetadata* vorbis = build_vorbis_comments(tags);
    FLAC__StreamMetadata* picture = nullptr;
    if (!vorbis) {
        set_error(error, "Failed to create vorbis comment metadata");
        g_object_unref(sink.tmp_file);
        g_object_unref(sink.file);
        g_object_unref(sink.temp_file);
        return 0;
    }
    if (has_cover_art_data(meta->cover_art)) {
        picture = build_picture_block(meta->cover_art);
        if (!picture) {
            set_error(error, "Failed to build picture metadata");
            g_object_unref(sink.tmp_file);
            g_object_unref(sink.file);
            g_object_unref(sink.temp_file);
            FLAC__metadata_object_delete(vorbis);
            return 0;
        }
    }
    std::vector<FLAC__StreamMetadata*> meta_blocks;
    meta_blocks.push_back(vorbis);
    if (picture) meta_blocks.push_back(picture);
    encoder.set_metadata(meta_blocks.data(), static_cast<unsigned>(meta_blocks.size()));

    auto init_and_handle = [&](std::string& out_err) -> bool {
        FLAC__StreamEncoderInitStatus st = encoder.init(sink.temp_path.c_str());
        if (st == FLAC__STREAM_ENCODER_INIT_STATUS_OK) return true;
        switch (st) {
            case FLAC__STREAM_ENCODER_INIT_STATUS_ENCODER_ERROR:
                out_err = "encoder error"; break;
            case FLAC__STREAM_ENCODER_INIT_STATUS_UNSUPPORTED_CONTAINER:
                out_err = "unsupported container"; break;
            case FLAC__STREAM_ENCODER_INIT_STATUS_INVALID_METADATA:
                out_err = "invalid metadata"; break;
            case FLAC__STREAM_ENCODER_INIT_STATUS_ALREADY_INITIALIZED:
                out_err = "encoder already initialized"; break;
            default:
                out_err = "init status " + std::to_string(static_cast<int>(st));
                break;
        }
        return false;
    };

    std::string init_err;
    if (!init_and_handle(init_err)) {
        std::string msg = "Failed to init FLAC stream encoder";
        if (!init_err.empty()) msg += ": " + init_err;
        set_error(error, msg);
        g_object_unref(sink.tmp_file);
        g_object_unref(sink.file);
        g_object_unref(sink.temp_file);
        FLAC__metadata_object_delete(vorbis);
        if (picture) FLAC__metadata_object_delete(picture);
        return 0;
    }

    auto cleanup_tmp = [&]() {
        GError* del_err = nullptr;
        if (sink.tmp_file) {
            g_file_delete(sink.tmp_file, nullptr, &del_err);
            g_clear_error(&del_err);
        }
        if (sink.temp_file) {
            g_file_delete(sink.temp_file, nullptr, &del_err);
            g_clear_error(&del_err);
        } else if (!sink.temp_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(sink.temp_path, ec);
        }
    };

    paranoia_seek(rip->paranoia, track->start, SEEK_SET);

    constexpr int kChunkSectors = 128;
    std::vector<FLAC__int32> left(kChunkSectors * kSamplesPerSector);
    std::vector<FLAC__int32> right(kChunkSectors * kSamplesPerSector);

    // wall-clock offset at track start (seconds since album start)
    const double wall_track_start =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count() / 1000.0 - wall_start_sec;

    long processed = 0;
    while (processed < sectors) {
        const int chunk = static_cast<int>(
            std::min<long>(kChunkSectors, sectors - processed));
        int read_sectors = 0;

        for (int c = 0; c < chunk; ++c) {
            int16_t* buffer = paranoia_read(rip->paranoia, nullptr);
            if (!buffer) {
                set_error(error,
                    "Read error on track " + std::to_string(track->number));
                encoder.finish();
                FLAC__metadata_object_delete(vorbis);
                if (picture) FLAC__metadata_object_delete(picture);
                cleanup_tmp();
                g_object_unref(sink.temp_file);
                g_object_unref(sink.tmp_file);
                g_object_unref(sink.file);
                return 0;
            }
            const int base = c * kSamplesPerSector;
            for (int i = 0; i < kSamplesPerSector; ++i) {
                left[base + i] = buffer[i * 2];
                right[base + i] = buffer[i * 2 + 1];
            }
            ++read_sectors;
        }

        const FLAC__int32* pcm[] = {left.data(), right.data()};
        const int samples_in_chunk = read_sectors * kSamplesPerSector;
        if (!encoder.process(pcm, samples_in_chunk)) {
            set_error(error,
                "FLAC encoding error on track " +
                std::to_string(track->number));
            encoder.finish();
            FLAC__metadata_object_delete(vorbis);
            if (picture) FLAC__metadata_object_delete(picture);
            cleanup_tmp();
            g_object_unref(sink.temp_file);
            g_object_unref(sink.tmp_file);
            g_object_unref(sink.file);
            return 0;
        }
        processed += read_sectors;
        const double pct = static_cast<double>(processed) / static_cast<double>(sectors) * 100.0;
        if (progress) {
            const double elapsed_track = static_cast<double>(processed) * kSamplesPerSector / kSampleRate;
            const double wall_now =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                .count() / 1000.0;
            const double wall_elapsed = wall_now - wall_start_sec;
            const double wall_track_elapsed = wall_now - wall_start_sec - wall_track_start;

            CdRipProgressInfo info{};
            info.track_number = track->number;
            info.total_tracks = total_tracks;
            info.percent = pct;
            info.elapsed_track_sec = elapsed_track;
            info.track_total_sec = static_cast<double>(sectors) * kSamplesPerSector / kSampleRate;
            info.elapsed_total_sec = completed_before_sec + elapsed_track;
            info.total_album_sec = total_album_sec;
            info.track_name = track_name.c_str();
            info.safe_title = safe_title.c_str();
            info.title = title.c_str();
            info.path = sink.final_uri.c_str();

            const double audio_done = info.elapsed_total_sec;
            const double audio_remain = std::max(0.0, total_album_sec - audio_done);
            const double throughput = (wall_elapsed > 0.0 && audio_done > 0.0)
                ? (audio_done / wall_elapsed) : 0.0;
            info.wall_elapsed_sec = wall_elapsed;

            if (throughput > 0.0) {
                const double est_total_wall =
                    wall_elapsed + audio_remain / throughput;
                info.wall_total_sec = est_total_wall;
                info.wall_track_total_sec =
                    (info.track_total_sec) / throughput;
            } else {
                info.wall_total_sec = 0.0;
                info.wall_track_total_sec = 0.0;
            }
            info.wall_track_elapsed_sec = wall_track_elapsed;
            progress(&info);
        }
    }

    encoder.finish();

    FLAC__metadata_object_delete(vorbis);
    if (picture) FLAC__metadata_object_delete(picture);


    GError* copy_err = nullptr;
    if (!g_file_copy(
        sink.temp_file,
        sink.tmp_file,
        G_FILE_COPY_OVERWRITE,
        nullptr,
        nullptr,
        nullptr,
        &copy_err)) {

        set_error(error,
            "Failed to copy to temporary destination "
            + sink.tmp_uri
            + " (" + (copy_err ? copy_err->message : "unknown") + ")");
        g_clear_error(&copy_err);
        cleanup_tmp();
        g_object_unref(sink.temp_file);
        g_object_unref(sink.tmp_file);
        g_object_unref(sink.file);
        return 0;
    }

    GError* move_err = nullptr;
    if (g_file_query_exists(sink.file, nullptr)) {
        g_file_delete(sink.file, nullptr, nullptr);
    }
    if (!g_file_move(
        sink.tmp_file,
        sink.file,
        G_FILE_COPY_OVERWRITE,
        nullptr,
        nullptr,
        nullptr,
        &move_err)) {

        set_error(error,
            "Failed to finalize file "
            + sink.final_uri
            + " (" + (move_err ? move_err->message : "unknown") + ")");
        g_clear_error(&move_err);
        cleanup_tmp();
        g_object_unref(sink.temp_file);
        g_object_unref(sink.tmp_file);
        g_object_unref(sink.file);
        return 0;
    }

    GError* del_temp_err = nullptr;
    g_file_delete(sink.temp_file, nullptr, &del_temp_err);
    g_clear_error(&del_temp_err);

    g_object_unref(sink.temp_file);
    g_object_unref(sink.tmp_file);
    g_object_unref(sink.file);
    return 1;
}

};
