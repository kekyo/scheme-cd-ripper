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

struct RealRipFixture {
    std::string filename{};
    std::map<std::string, std::string> tags{};
    std::string expected_cddb_discid{};
    std::string expected_mb_discid{};
    std::string expected_mb_release{};
    std::string expected_mb_medium{};
    int expected_track_number{0};
    int expected_track_total{0};
    long expected_leadout_sector{0};
    int expected_length_seconds{0};
    std::vector<long> expected_offsets{};
};

struct InvalidTaggedTocFixture {
    std::string filename{};
    std::map<std::string, std::string> tags{};
    std::string expected_reason{};
};

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

auto expect_size = [](
    size_t expected,
    size_t actual,
    const std::string& message) {

    if (expected != actual) {
        std::cerr << "assert_size failed: " << message << "\n";
        std::cerr << "  expected: " << expected << "\n";
        std::cerr << "  actual:   " << actual << "\n";
        std::exit(1);
    }
};

auto expect_null = [](
    const void* value,
    const std::string& message) {

    if (value != nullptr) {
        std::cerr << "assert_null failed: " << message << "\n";
        std::exit(1);
    }
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

auto find_item_by_discid = [](
    const CdRipTaggedTocList* list,
    const std::string& cddb_discid) -> const CdRipTaggedToc* {

    expect_true(list != nullptr, "tagged TOC list should exist");
    for (size_t i = 0; i < list->count; ++i) {
        const auto* toc = list->items[i].toc;
        if (!toc) continue;
        if (cdrip::detail::to_string_or_empty(toc->cddb_discid) == cddb_discid) {
            return &list->items[i];
        }
    }
    return static_cast<const CdRipTaggedToc*>(nullptr);
};

auto find_item_by_filename = [](
    const CdRipTaggedTocList* list,
    const std::string& filename) -> const CdRipTaggedToc* {

    expect_true(list != nullptr, "tagged TOC list should exist");
    for (size_t i = 0; i < list->count; ++i) {
        const std::filesystem::path path{
            cdrip::detail::to_string_or_empty(list->items[i].path)};
        if (path.filename().string() == filename) {
            return &list->items[i];
        }
    }
    return static_cast<const CdRipTaggedToc*>(nullptr);
};

auto assert_item_matches_fixture = [](
    const CdRipTaggedToc& item,
    const RealRipFixture& fixture) {

    expect_true(item.valid != 0, "fixture-derived tagged TOC should be valid");
    expect_true(item.reason == nullptr, "valid tagged TOC should not have an error reason");
    expect_true(item.toc != nullptr, "valid tagged TOC should include a reconstructed TOC");
    expect_true(item.track_number == fixture.expected_track_number, "track number should be reconstructed from tags");

    const auto* toc = item.toc;
    expect_eq(
        fixture.expected_cddb_discid,
        cdrip::detail::to_string_or_empty(toc->cddb_discid),
        "CDDB disc id should match the real rip tags");
    expect_eq(
        fixture.expected_mb_discid,
        cdrip::detail::to_string_or_empty(toc->mb_discid),
        "MusicBrainz disc id should match the real rip tags");
    expect_eq(
        fixture.expected_mb_release,
        cdrip::detail::to_string_or_empty(toc->mb_release_id),
        "MusicBrainz release id should match the real rip tags");
    expect_eq(
        fixture.expected_mb_medium,
        cdrip::detail::to_string_or_empty(toc->mb_medium_id),
        "MusicBrainz medium id should match the real rip tags");
    expect_true(
        toc->leadout_sector == fixture.expected_leadout_sector,
        "leadout sector should match the real rip tags");
    expect_true(
        toc->length_seconds == fixture.expected_length_seconds,
        "disc length should match the real rip tags");
    expect_size(
        static_cast<size_t>(fixture.expected_track_total),
        toc->tracks_count,
        "track total should match the real rip tags");

    const long disc_frames =
        static_cast<long>(fixture.expected_length_seconds) * CDIO_CD_FRAMES_PER_SEC;
    for (size_t i = 0; i < fixture.expected_offsets.size(); ++i) {
        expect_true(
            toc->tracks[i].number == static_cast<int>(i + 1),
            "reconstructed tracks should be renumbered consecutively");
        expect_true(
            toc->tracks[i].start == fixture.expected_offsets[i],
            "reconstructed track start should match the real rip offsets");
        const long expected_end =
            (i + 1 < fixture.expected_offsets.size())
                ? fixture.expected_offsets[i + 1] - 1
                : disc_frames - 1;
        expect_true(
            toc->tracks[i].end == expected_end,
            "reconstructed track end should follow the encoded offsets");
        expect_true(toc->tracks[i].is_audio != 0, "reconstructed tagged TOC should describe audio tracks");
    }
};

auto make_real_rip_fixtures = []() {
    // These tag-only fixtures were copied from local FLACs under /home/kouji/Music.
    // They intentionally preserve only disc lookup metadata, not audio content.
    return std::vector<RealRipFixture>{
        RealRipFixture{
            "real-rip-gold-track19.flac",
            {
                {"CDDB_DISCID", "3128b13"},
                {"CDDB_OFFSETS", "0,17482,35768,54429,70401,90989,110023,131438,153734,167893,183145,207738,226845,250004,271841,286508,304251,326335,343631"},
                {"CDDB_TOTAL_SECONDS", "4747"},
                {"MUSICBRAINZ_DISCID", "JccSw1uJ4N1gVYL6pc3GfkTluOM-"},
                {"MUSICBRAINZ_LEADOUT", "356216"},
                {"MUSICBRAINZ_MEDIUM", "3985a666-01a1-3478-951a-d0e1369896c8"},
                {"MUSICBRAINZ_RELEASE", "bc0048d9-4a60-487b-a8d0-0dac0a6d19e4"},
                {"TRACKNUMBER", "19"},
                {"TRACKTOTAL", "19"},
            },
            "3128b13",
            "JccSw1uJ4N1gVYL6pc3GfkTluOM-",
            "bc0048d9-4a60-487b-a8d0-0dac0a6d19e4",
            "3985a666-01a1-3478-951a-d0e1369896c8",
            19,
            19,
            356066,
            4747,
            {0, 17482, 35768, 54429, 70401, 90989, 110023, 131438, 153734, 167893, 183145, 207738, 226845, 250004, 271841, 286508, 304251, 326335, 343631},
        },
        RealRipFixture{
            "real-rip-gold-track19-recompute-discid.flac",
            {
                {"CDDB_DISCID", "3128b13"},
                {"CDDB_OFFSETS", "0,17482,35768,54429,70401,90989,110023,131438,153734,167893,183145,207738,226845,250004,271841,286508,304251,326335,343631"},
                {"CDDB_TOTAL_SECONDS", "4747"},
                {"MUSICBRAINZ_LEADOUT", "356216"},
                {"MUSICBRAINZ_MEDIUM", "3985a666-01a1-3478-951a-d0e1369896c8"},
                {"MUSICBRAINZ_RELEASE", "bc0048d9-4a60-487b-a8d0-0dac0a6d19e4"},
                {"TRACKNUMBER", "19"},
                {"TRACKTOTAL", "19"},
            },
            "3128b13",
            "JccSw1uJ4N1gVYL6pc3GfkTluOM-",
            "bc0048d9-4a60-487b-a8d0-0dac0a6d19e4",
            "3985a666-01a1-3478-951a-d0e1369896c8",
            19,
            19,
            356066,
            4747,
            {0, 17482, 35768, 54429, 70401, 90989, 110023, 131438, 153734, 167893, 183145, 207738, 226845, 250004, 271841, 286508, 304251, 326335, 343631},
        },
        RealRipFixture{
            "real-rip-kylie-track10.flac",
            {
                {"CDDB_DISCID", "8808500a"},
                {"CDDB_OFFSETS", "0,15350,29922,48022,65862,80887,97180,111805,129057,145285"},
                {"CDDB_TOTAL_SECONDS", "2128"},
                {"MUSICBRAINZ_MEDIUM", "22081af6-094d-31bc-bd01-10ccd6cdee34"},
                {"MUSICBRAINZ_RELEASE", "338005d3-ea26-3daa-bb44-108ec28568c4"},
                {"TRACKNUMBER", "10"},
                {"TRACKTOTAL", "10"},
            },
            "8808500a",
            "",
            "338005d3-ea26-3daa-bb44-108ec28568c4",
            "22081af6-094d-31bc-bd01-10ccd6cdee34",
            10,
            10,
            159600,
            2128,
            {0, 15350, 29922, 48022, 65862, 80887, 97180, 111805, 129057, 145285},
        },
    };
};

auto make_invalid_tagged_toc_fixtures = []() {
    return std::vector<InvalidTaggedTocFixture>{
        InvalidTaggedTocFixture{
            "invalid-offset-token.flac",
            {
                {"CDDB_DISCID", "deadbeef"},
                {"CDDB_OFFSETS", "0,15000,nope"},
                {"CDDB_TOTAL_SECONDS", "600"},
                {"TRACKNUMBER", "1"},
                {"TRACKTOTAL", "3"},
            },
            "Invalid CDDB_OFFSETS",
        },
        InvalidTaggedTocFixture{
            "offsets-not-increasing.flac",
            {
                {"CDDB_DISCID", "deadbeef"},
                {"CDDB_OFFSETS", "0,15000,14999"},
                {"CDDB_TOTAL_SECONDS", "600"},
                {"TRACKNUMBER", "1"},
                {"TRACKTOTAL", "3"},
            },
            "Offsets are not strictly increasing",
        },
        InvalidTaggedTocFixture{
            "tracktotal-mismatch.flac",
            {
                {"CDDB_DISCID", "deadbeef"},
                {"CDDB_OFFSETS", "0,15000,27000"},
                {"CDDB_TOTAL_SECONDS", "600"},
                {"TRACKNUMBER", "1"},
                {"TRACKTOTAL", "2"},
            },
            "Offsets count mismatch with track total",
        },
        InvalidTaggedTocFixture{
            "missing-total-seconds.flac",
            {
                {"CDDB_DISCID", "deadbeef"},
                {"CDDB_OFFSETS", "0,15000,27000"},
                {"TRACKNUMBER", "1"},
                {"TRACKTOTAL", "3"},
            },
            "Missing CDDB tags",
        },
    };
};

auto assert_invalid_item_matches_fixture = [](
    const CdRipTaggedToc& item,
    const InvalidTaggedTocFixture& fixture) {

    expect_true(item.valid == 0, "invalid fixture should remain invalid");
    expect_null(item.toc, "invalid fixture should not create a TOC");
    expect_eq(
        fixture.expected_reason,
        cdrip::detail::to_string_or_empty(item.reason),
        "invalid fixture should report the expected reason");
};

auto prepare_fixture_directory = [](
    const std::filesystem::path& temp_dir,
    const std::vector<RealRipFixture>& valid_fixtures,
    const std::vector<InvalidTaggedTocFixture>& invalid_fixtures) {

    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    for (size_t i = 0; i < valid_fixtures.size(); ++i) {
        const auto fixture_dir = temp_dir / ("valid-disc-" + std::to_string(i + 1));
        std::filesystem::create_directories(fixture_dir);
        write_test_flac(
            (fixture_dir / valid_fixtures[i].filename).string(),
            valid_fixtures[i].tags);
    }
    for (size_t i = 0; i < invalid_fixtures.size(); ++i) {
        const auto fixture_dir = temp_dir / ("invalid-disc-" + std::to_string(i + 1));
        std::filesystem::create_directories(fixture_dir);
        write_test_flac((fixture_dir / invalid_fixtures[i].filename).string(), invalid_fixtures[i].tags);
    }
};

auto test_collect_cddb_queries_reconstructs_real_rip_tag_metadata = []() {
    const auto fixtures = make_real_rip_fixtures();
    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-real-rip-tags";
    prepare_fixture_directory(temp_dir, fixtures, {});

    const char* err = nullptr;
    CdRipTaggedTocList* list = cdrip_collect_cddb_queries_from_path(temp_dir.c_str(), &err);
    expect_true(list != nullptr, "collect should always return a list object");
    expect_true(err == nullptr, err ? err : "collect should succeed for fixture directory");
    expect_size(fixtures.size(), list->count, "fixture directory should produce one tagged TOC per FLAC");

    for (const auto& fixture : fixtures) {
        const CdRipTaggedToc* item = find_item_by_discid(list, fixture.expected_cddb_discid);
        expect_true(item != nullptr, "fixture disc id should be present in collected tagged TOCs");
        assert_item_matches_fixture(*item, fixture);
    }

    cdrip_release_taggedtoc_list(list);
    std::filesystem::remove_all(temp_dir);
};

auto test_collect_cddb_queries_marks_invalid_tagged_tocs = []() {
    const auto valid_fixtures = make_real_rip_fixtures();
    const auto invalid_fixtures = make_invalid_tagged_toc_fixtures();
    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-invalid-real-rip-tags";
    prepare_fixture_directory(temp_dir, valid_fixtures, invalid_fixtures);

    const char* err = nullptr;
    CdRipTaggedTocList* list = cdrip_collect_cddb_queries_from_path(temp_dir.c_str(), &err);
    expect_true(list != nullptr, "collect should always return a list object");
    expect_true(err == nullptr, err ? err : "collect should succeed even with invalid tagged TOCs");
    expect_size(
        valid_fixtures.size() + invalid_fixtures.size(),
        list->count,
        "collect should preserve both valid and invalid tagged TOCs");

    for (const auto& fixture : valid_fixtures) {
        const CdRipTaggedToc* item = find_item_by_discid(list, fixture.expected_cddb_discid);
        expect_true(item != nullptr, "valid fixture should still be present");
        assert_item_matches_fixture(*item, fixture);
    }
    for (const auto& fixture : invalid_fixtures) {
        const CdRipTaggedToc* item = find_item_by_filename(list, fixture.filename);
        expect_true(item != nullptr, "invalid fixture should still be present");
        assert_invalid_item_matches_fixture(*item, fixture);
    }

    cdrip_release_taggedtoc_list(list);
    std::filesystem::remove_all(temp_dir);
};

auto test_collect_cddb_queries_falls_back_when_musicbrainz_leadout_is_malformed = []() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-real-rip-tags-malformed-leadout";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    RealRipFixture fixture{
        "real-rip-gold-track19-malformed-leadout.flac",
        {
            {"CDDB_DISCID", "3128b13"},
            {"CDDB_OFFSETS", "0,17482,35768,54429,70401,90989,110023,131438,153734,167893,183145,207738,226845,250004,271841,286508,304251,326335,343631"},
            {"CDDB_TOTAL_SECONDS", "4747"},
            {"MUSICBRAINZ_LEADOUT", "invalid"},
            {"MUSICBRAINZ_MEDIUM", "3985a666-01a1-3478-951a-d0e1369896c8"},
            {"MUSICBRAINZ_RELEASE", "bc0048d9-4a60-487b-a8d0-0dac0a6d19e4"},
            {"TRACKNUMBER", "19"},
            {"TRACKTOTAL", "19"},
        },
        "3128b13",
        "",
        "bc0048d9-4a60-487b-a8d0-0dac0a6d19e4",
        "3985a666-01a1-3478-951a-d0e1369896c8",
        19,
        19,
        4747L * CDIO_CD_FRAMES_PER_SEC,
        4747,
        {0, 17482, 35768, 54429, 70401, 90989, 110023, 131438, 153734, 167893, 183145, 207738, 226845, 250004, 271841, 286508, 304251, 326335, 343631},
    };

    CdRipDiscToc expected_toc{};
    std::vector<CdRipTrackInfo> expected_tracks;
    expected_tracks.reserve(fixture.expected_offsets.size());
    for (size_t i = 0; i < fixture.expected_offsets.size(); ++i) {
        const long end =
            (i + 1 < fixture.expected_offsets.size())
                ? fixture.expected_offsets[i + 1] - 1
                : fixture.expected_leadout_sector - 1;
        expected_tracks.push_back(CdRipTrackInfo{
            static_cast<int>(i + 1),
            fixture.expected_offsets[i],
            end,
            1,
        });
    }
    expected_toc.tracks = expected_tracks.data();
    expected_toc.tracks_count = expected_tracks.size();
    expected_toc.leadout_sector = fixture.expected_leadout_sector;
    expected_toc.length_seconds = fixture.expected_length_seconds;
    std::string expected_mb_discid;
    long expected_mb_leadout = 0;
    expect_true(
        cdrip::detail::compute_musicbrainz_discid(&expected_toc, expected_mb_discid, expected_mb_leadout),
        "fallback MusicBrainz disc id should be computable");

    write_test_flac((temp_dir / fixture.filename).string(), fixture.tags);
    const char* err = nullptr;
    CdRipTaggedTocList* list = cdrip_collect_cddb_queries_from_path(temp_dir.c_str(), &err);
    expect_true(list != nullptr, "collect should always return a list object");
    expect_true(err == nullptr, err ? err : "collect should succeed for malformed leadout fallback fixture");
    expect_size(1, list->count, "malformed leadout fixture should still produce one tagged TOC");

    const CdRipTaggedToc* item = find_item_by_filename(list, fixture.filename);
    expect_true(item != nullptr, "malformed leadout fixture should be present");
    expect_true(item->valid != 0, "malformed leadout fixture should still be treated as valid");
    expect_true(item->toc != nullptr, "malformed leadout fixture should reconstruct a TOC");
    expect_true(
        item->toc->leadout_sector == fixture.expected_leadout_sector,
        "malformed leadout should fall back to disc length derived frames");
    expect_eq(
        expected_mb_discid,
        cdrip::detail::to_string_or_empty(item->toc->mb_discid),
        "malformed leadout should still allow fallback MusicBrainz disc id reconstruction");

    cdrip_release_taggedtoc_list(list);
    std::filesystem::remove_all(temp_dir);
};

}  // namespace

int main() {
    test_collect_cddb_queries_reconstructs_real_rip_tag_metadata();
    test_collect_cddb_queries_marks_invalid_tagged_tocs();
    test_collect_cddb_queries_falls_back_when_musicbrainz_leadout_is_malformed();
    return 0;
}
