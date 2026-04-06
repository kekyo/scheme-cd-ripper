#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <FLAC++/encoder.h>

#include "../src/cdrip/internal.h"

namespace {

constexpr unsigned int kChannels = 2;
constexpr unsigned int kSampleRate = 44100;
constexpr unsigned int kSamples = 1024;

auto expect_true = [](
    bool condition,
    const std::string& message) {

    if (!condition) {
        std::cerr << "assert_true failed: " << message << "\n";
        std::exit(1);
    }
};

auto expect_eq = [](
    const std::string& expected,
    const std::string& actual,
    const std::string& message) {

    if (expected != actual) {
        std::cerr << "assert_eq failed: " << message << "\n";
        std::cerr << "  expected: " << expected << "\n";
        std::cerr << "  actual:   " << actual << "\n";
        std::exit(1);
    }
};

auto read_vorbis_comments = [](
    const std::string& path) {

    std::map<std::string, std::string> out;
    FLAC__StreamMetadata* tags = nullptr;
    expect_true(FLAC__metadata_get_tags(path.c_str(), &tags), "FLAC metadata should be readable");
    expect_true(tags && tags->type == FLAC__METADATA_TYPE_VORBIS_COMMENT, "Vorbis comment block should exist");

    const auto& vc = tags->data.vorbis_comment;
    for (uint32_t i = 0; i < vc.num_comments; ++i) {
        char* name = nullptr;
        char* value = nullptr;
        if (FLAC__metadata_object_vorbiscomment_entry_to_name_value_pair(
                vc.comments[i],
                &name,
                &value)) {
            out[cdrip::detail::to_upper(cdrip::detail::to_string_or_empty(name))] =
                cdrip::detail::to_string_or_empty(value);
        }
        if (name) std::free(name);
        if (value) std::free(value);
    }
    FLAC__metadata_object_delete(tags);
    return out;
};

auto write_test_flac = [](
    const std::string& path,
    const std::map<std::string, std::string>& tags) {

    FLAC::Encoder::File encoder;
    encoder.set_verify(false);
    encoder.set_compression_level(1);
    encoder.set_channels(kChannels);
    encoder.set_bits_per_sample(16);
    encoder.set_sample_rate(kSampleRate);
    encoder.set_total_samples_estimate(kSamples);

    FLAC__StreamMetadata* vorbis = cdrip::detail::build_vorbis_comments(tags);
    expect_true(vorbis != nullptr, "Vorbis block should be created");
    std::vector<FLAC__StreamMetadata*> blocks{vorbis};
    encoder.set_metadata(blocks.data(), static_cast<unsigned>(blocks.size()));

    const auto init_status = encoder.init(path.c_str());
    expect_true(
        init_status == FLAC__STREAM_ENCODER_INIT_STATUS_OK,
        "FLAC encoder should initialize");

    std::vector<FLAC__int32> left(kSamples, 0);
    std::vector<FLAC__int32> right(kSamples, 0);
    const FLAC__int32* pcm[] = {left.data(), right.data()};
    expect_true(encoder.process(pcm, kSamples), "FLAC encoder should accept PCM");
    expect_true(encoder.finish(), "FLAC encoder should finish");
    FLAC__metadata_object_delete(vorbis);
};

auto make_test_toc = []() {
    static CdRipTrackInfo tracks[] = {
        CdRipTrackInfo{1, 0, 14999, 1},
    };
    CdRipDiscToc toc{};
    toc.cddb_discid = "deadbeef";
    toc.tracks = tracks;
    toc.tracks_count = 1;
    toc.leadout_sector = tracks[0].end + 1;
    toc.length_seconds = static_cast<int>(toc.leadout_sector / 75);
    return toc;
};

auto make_test_entry = []() {
    static CdRipTagKV album_tags[] = {
        CdRipTagKV{"ARTIST", "Test Artist"},
        CdRipTagKV{"ALBUM", "Test Album"},
        CdRipTagKV{"DATE", "2024"},
    };
    static CdRipTagKV track_tags[] = {
        CdRipTagKV{"TITLE", "Updated Title"},
    };
    static CdRipTrackTags tracks[] = {
        CdRipTrackTags{track_tags, 1},
    };

    CdRipCddbEntry entry{};
    entry.cddb_discid = "deadbeef";
    entry.source_label = "musicbrainz";
    entry.source_url = "https://example.invalid/release";
    entry.fetched_at = "2024-01-01T00:00:00+00:00";
    entry.album_tags = album_tags;
    entry.album_tags_count = sizeof(album_tags) / sizeof(album_tags[0]);
    entry.tracks = tracks;
    entry.tracks_count = sizeof(tracks) / sizeof(tracks[0]);
    return entry;
};

auto test_build_replaygain_tags_formats_values = []() {
    cdrip::detail::ReplayGainScanResult track{};
    track.loudness_lufs = -20.0;
    track.peak = 0.75000000;
    track.loudness_ok = true;
    track.peak_ok = true;

    cdrip::detail::ReplayGainScanResult album{};
    album.loudness_lufs = -18.5;
    album.peak = 0.80000000;
    album.loudness_ok = true;
    album.peak_ok = true;

    const auto tags = cdrip::detail::build_replaygain_tags(track, album);
    expect_eq("+2.00 dB", tags.at("REPLAYGAIN_TRACK_GAIN"), "track gain should target -18 LUFS");
    expect_eq("0.75000000", tags.at("REPLAYGAIN_TRACK_PEAK"), "track peak should be formatted");
    expect_eq("+0.50 dB", tags.at("REPLAYGAIN_ALBUM_GAIN"), "album gain should target -18 LUFS");
    expect_eq("0.80000000", tags.at("REPLAYGAIN_ALBUM_PEAK"), "album peak should be formatted");
};

auto test_update_mode_preserves_existing_replaygain_tags = []() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-replaygain-preserve";
    std::filesystem::create_directories(temp_dir);
    const auto flac_path = (temp_dir / "preserve.flac").string();

    write_test_flac(
        flac_path,
        {
            {"TITLE", "Original Title"},
            {"REPLAYGAIN_TRACK_GAIN", "-7.10 dB"},
            {"REPLAYGAIN_TRACK_PEAK", "0.91000000"},
            {"REPLAYGAIN_ALBUM_GAIN", "-6.20 dB"},
            {"REPLAYGAIN_ALBUM_PEAK", "0.93000000"},
        });

    const auto toc = make_test_toc();
    const auto entry = make_test_entry();
    CdRipTaggedToc tagged{};
    tagged.path = flac_path.c_str();
    tagged.toc = const_cast<CdRipDiscToc*>(&toc);
    tagged.track_number = 1;
    tagged.valid = 1;

    const char* err = nullptr;
    expect_true(
        cdrip_update_flac_with_cddb_entry(&tagged, &entry, &err) == 1,
        err ? err : "update should succeed");
    cdrip_release_error(err);

    const auto tags = read_vorbis_comments(flac_path);
    expect_eq("Updated Title", tags.at("TITLE"), "title should be refreshed");
    expect_eq("Test Artist", tags.at("ARTIST"), "artist should be refreshed");
    expect_eq("-7.10 dB", tags.at("REPLAYGAIN_TRACK_GAIN"), "track gain should be preserved");
    expect_eq("0.91000000", tags.at("REPLAYGAIN_TRACK_PEAK"), "track peak should be preserved");
    expect_eq("-6.20 dB", tags.at("REPLAYGAIN_ALBUM_GAIN"), "album gain should be preserved");
    expect_eq("0.93000000", tags.at("REPLAYGAIN_ALBUM_PEAK"), "album peak should be preserved");

    std::filesystem::remove_all(temp_dir);
};

auto test_update_flac_tags_overrides_replaygain_values = []() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-replaygain-override";
    std::filesystem::create_directories(temp_dir);
    const auto flac_path = (temp_dir / "override.flac").string();

    write_test_flac(
        flac_path,
        {
            {"TITLE", "Original Title"},
            {"REPLAYGAIN_TRACK_GAIN", "-7.10 dB"},
            {"REPLAYGAIN_TRACK_PEAK", "0.91000000"},
            {"REPLAYGAIN_ALBUM_GAIN", "-6.20 dB"},
            {"REPLAYGAIN_ALBUM_PEAK", "0.93000000"},
        });

    const auto toc = make_test_toc();
    const auto entry = make_test_entry();
    std::string err;
    expect_true(
        cdrip::detail::update_flac_tags(
            flac_path,
            &toc,
            1,
            &entry,
            {
                {"REPLAYGAIN_TRACK_GAIN", "+1.50 dB"},
                {"REPLAYGAIN_TRACK_PEAK", "0.45000000"},
                {"REPLAYGAIN_ALBUM_GAIN", "+1.00 dB"},
                {"REPLAYGAIN_ALBUM_PEAK", "0.50000000"},
            },
            /*preserve_replaygain_tags=*/false,
            err),
        err.empty() ? "ReplayGain update should succeed" : err);

    const auto tags = read_vorbis_comments(flac_path);
    expect_eq("+1.50 dB", tags.at("REPLAYGAIN_TRACK_GAIN"), "track gain should be replaced");
    expect_eq("0.45000000", tags.at("REPLAYGAIN_TRACK_PEAK"), "track peak should be replaced");
    expect_eq("+1.00 dB", tags.at("REPLAYGAIN_ALBUM_GAIN"), "album gain should be replaced");
    expect_eq("0.50000000", tags.at("REPLAYGAIN_ALBUM_PEAK"), "album peak should be replaced");

    std::filesystem::remove_all(temp_dir);
};

}

int main() {
    test_build_replaygain_tags_formats_values();
    test_update_mode_preserves_existing_replaygain_tags();
    test_update_flac_tags_overrides_replaygain_values();
    return 0;
}
