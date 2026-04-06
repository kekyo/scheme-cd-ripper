// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include <cdio/cdio.h>
#include <cdio/cd_types.h>
#include <FLAC++/encoder.h>
#include <gio/gio.h>
#include <glib.h>

#include "internal.h"

using namespace cdrip::detail;

namespace {

constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;
constexpr int kSampleRate = 44100;
constexpr int kSamplesPerSector = CDIO_CD_FRAMESIZE_RAW / (kChannels * sizeof(int16_t));

bool is_uri(
    const std::string& path) {

    return path.find("://") != std::string::npos;
}

bool create_local_temp_file(
    std::string& out_path,
    std::string& err) {

    err.clear();
    GError* gerr = nullptr;
    gchar* temp_path_c = nullptr;
    const int temp_fd = g_file_open_tmp("cdripXXXXXX.flac", &temp_path_c, &gerr);
    if (temp_fd == -1) {
        err = std::string("Failed to create temporary file: ") +
            (gerr ? gerr->message : "unknown");
        g_clear_error(&gerr);
        return false;
    }
    close(temp_fd);
    out_path = temp_path_c ? temp_path_c : "";
    g_free(temp_path_c);
    if (out_path.empty()) {
        err = "Failed to determine temporary file path";
        return false;
    }
    return true;
}

void remove_local_file_quietly(
    const std::string& path) {

    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}

namespace cdrip::detail {

bool publish_local_file_to_destination(
    const std::string& local_path,
    const std::string& destination_path,
    std::string& err) {

    err.clear();
    if (local_path.empty() || destination_path.empty()) {
        err = "Invalid source or destination path";
        return false;
    }

    const bool uri_output = is_uri(destination_path);
    const std::string tmp_destination = destination_path + ".tmp";

    GFile* src_file = g_file_new_for_path(local_path.c_str());
    GFile* file = uri_output
        ? g_file_new_for_uri(destination_path.c_str())
        : g_file_new_for_path(destination_path.c_str());
    GFile* tmp_file = uri_output
        ? g_file_new_for_uri(tmp_destination.c_str())
        : g_file_new_for_path(tmp_destination.c_str());

    auto cleanup = [&]() {
        if (src_file) g_object_unref(src_file);
        if (tmp_file) g_object_unref(tmp_file);
        if (file) g_object_unref(file);
    };

    GError* gerr = nullptr;
    GFile* parent = g_file_get_parent(tmp_file);
    if (parent) {
        if (!g_file_make_directory_with_parents(parent, nullptr, &gerr)) {
            if (gerr && gerr->code != G_IO_ERROR_EXISTS) {
                err = std::string("Failed to create directories for ")
                    + tmp_destination
                    + ": "
                    + (gerr ? gerr->message : "unknown");
                g_clear_error(&gerr);
                g_object_unref(parent);
                cleanup();
                return false;
            }
            g_clear_error(&gerr);
        }
        g_object_unref(parent);
    }

    if (!g_file_copy(
            src_file,
            tmp_file,
            G_FILE_COPY_OVERWRITE,
            nullptr,
            nullptr,
            nullptr,
            &gerr)) {
        err = std::string("Failed to copy to temporary destination ")
            + tmp_destination
            + " ("
            + (gerr ? gerr->message : "unknown")
            + ")";
        g_clear_error(&gerr);
        cleanup();
        return false;
    }

    if (g_file_query_exists(file, nullptr)) {
        g_file_delete(file, nullptr, nullptr);
    }
    if (!g_file_move(
            tmp_file,
            file,
            G_FILE_COPY_OVERWRITE,
            nullptr,
            nullptr,
            nullptr,
            &gerr)) {
        err = std::string("Failed to finalize file ")
            + destination_path
            + " ("
            + (gerr ? gerr->message : "unknown")
            + ")";
        g_clear_error(&gerr);
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

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
    std::string& err) {

    err.clear();
    if (replaygain_result) {
        *replaygain_result = ReplayGainScanResult{};
    }
    if (!rip) {
        err = "Drive handle is null";
        return false;
    }
    if (!track || !meta || !toc) {
        err = "Invalid arguments to cdrip_rip_track";
        return false;
    }
    if (!track->is_audio) {
        std::cout << "Skipping data track " << track->number << "\n";
        return true;
    }

    const long sectors = track->end - track->start + 1;
    if (sectors <= 0) {
        err = "Track " + std::to_string(track->number) + " has invalid length";
        return false;
    }

    std::string title;
    std::string track_name;
    std::string safe_title;
    std::map<std::string, std::string> tags = build_track_vorbis_tags(
        track,
        meta,
        toc,
        total_tracks,
        title,
        track_name,
        safe_title);

    std::string output_path;
    if (options && options->output_path && options->output_path[0]) {
        output_path = options->output_path;
    } else if (!resolve_track_output_path(rip->format, tags, output_path, err)) {
        return false;
    }

    const std::string display_path =
        (options && options->display_path && options->display_path[0])
            ? std::string{options->display_path}
            : output_path;

    std::string temp_path;
    if (!create_local_temp_file(temp_path, err)) {
        return false;
    }

    int compression_level = rip->compression_level;
    if (compression_level < 0) {
        compression_level = (rip->mode == RIP_MODES_FAST) ? 1 : 5;
    }

    cdda_speed_set(rip->drive, rip->speed_fast ? 0 : 1);

    FLAC::Encoder::File encoder;
    encoder.set_verify(false);
    encoder.set_compression_level(compression_level);
    encoder.set_channels(kChannels);
    encoder.set_bits_per_sample(kBitsPerSample);
    encoder.set_sample_rate(kSampleRate);
    encoder.set_total_samples_estimate(
        static_cast<uint64_t>(sectors) * kSamplesPerSector);

    std::map<std::string, std::string> vorbis_tags = tags;
    drop_format_only_tags(vorbis_tags);
    FLAC__StreamMetadata* vorbis = build_vorbis_comments(vorbis_tags);
    FLAC__StreamMetadata* picture = nullptr;
    if (!vorbis) {
        err = "Failed to create vorbis comment metadata";
        remove_local_file_quietly(temp_path);
        return false;
    }
    if (has_cover_art_data(meta->cover_art)) {
        picture = build_picture_block(meta->cover_art);
        if (!picture) {
            err = "Failed to build picture metadata";
            FLAC__metadata_object_delete(vorbis);
            remove_local_file_quietly(temp_path);
            return false;
        }
    }
    std::vector<FLAC__StreamMetadata*> meta_blocks;
    meta_blocks.push_back(vorbis);
    if (picture) meta_blocks.push_back(picture);
    encoder.set_metadata(meta_blocks.data(), static_cast<unsigned>(meta_blocks.size()));

    auto cleanup_encoder_state = [&]() {
        FLAC__metadata_object_delete(vorbis);
        if (picture) FLAC__metadata_object_delete(picture);
    };

    FLAC__StreamEncoderInitStatus init_status = encoder.init(temp_path.c_str());
    if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        err = "Failed to init FLAC stream encoder: init status "
            + std::to_string(static_cast<int>(init_status));
        cleanup_encoder_state();
        remove_local_file_quietly(temp_path);
        return false;
    }

    paranoia_seek(rip->paranoia, track->start, SEEK_SET);

    constexpr int kChunkSectors = 128;
    std::vector<FLAC__int32> left(kChunkSectors * kSamplesPerSector);
    std::vector<FLAC__int32> right(kChunkSectors * kSamplesPerSector);

    const double wall_track_start =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count() / 1000.0
        - wall_start_sec;

    long processed = 0;
    while (processed < sectors) {
        const int chunk = static_cast<int>(
            std::min<long>(kChunkSectors, sectors - processed));
        int read_sectors = 0;

        for (int c = 0; c < chunk; ++c) {
            int16_t* buffer = paranoia_read(rip->paranoia, nullptr);
            if (!buffer) {
                err = "Read error on track " + std::to_string(track->number);
                encoder.finish();
                cleanup_encoder_state();
                remove_local_file_quietly(temp_path);
                return false;
            }

            if (options && options->track_replaygain_state) {
                if (ebur128_add_frames_short(options->track_replaygain_state, buffer, kSamplesPerSector) != EBUR128_SUCCESS) {
                    err = "Failed to update ReplayGain track state";
                    encoder.finish();
                    cleanup_encoder_state();
                    remove_local_file_quietly(temp_path);
                    return false;
                }
            }
            if (options && options->album_replaygain_state) {
                if (ebur128_add_frames_short(options->album_replaygain_state, buffer, kSamplesPerSector) != EBUR128_SUCCESS) {
                    err = "Failed to update ReplayGain album state";
                    encoder.finish();
                    cleanup_encoder_state();
                    remove_local_file_quietly(temp_path);
                    return false;
                }
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
            err = "FLAC encoding error on track " + std::to_string(track->number);
            encoder.finish();
            cleanup_encoder_state();
            remove_local_file_quietly(temp_path);
            return false;
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
            info.path = display_path.c_str();

            const double audio_done = info.elapsed_total_sec;
            const double audio_remain = std::max(0.0, total_album_sec - audio_done);
            const double throughput = (wall_elapsed > 0.0 && audio_done > 0.0)
                ? (audio_done / wall_elapsed)
                : 0.0;
            info.wall_elapsed_sec = wall_elapsed;

            if (throughput > 0.0) {
                info.wall_total_sec = wall_elapsed + audio_remain / throughput;
                info.wall_track_total_sec = info.track_total_sec / throughput;
            } else {
                info.wall_total_sec = 0.0;
                info.wall_track_total_sec = 0.0;
            }
            info.wall_track_elapsed_sec = wall_track_elapsed;
            progress(&info);
        }
    }

    encoder.finish();
    cleanup_encoder_state();

    if (replaygain_result && options && options->track_replaygain_state) {
        if (!finalize_replaygain_scan(options->track_replaygain_state, *replaygain_result, err)) {
            remove_local_file_quietly(temp_path);
            return false;
        }
    }

    if (!publish_local_file_to_destination(temp_path, output_path, err)) {
        remove_local_file_quietly(temp_path);
        return false;
    }
    remove_local_file_quietly(temp_path);
    return true;
}

}  // namespace cdrip::detail

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
    std::string err;
    if (!rip_track_with_options(
            rip,
            track,
            meta,
            toc,
            progress,
            total_tracks,
            completed_before_sec,
            total_album_sec,
            wall_start_sec,
            nullptr,
            nullptr,
            err)) {
        set_error(error, err);
        return 0;
    }
    return 1;
}

}
