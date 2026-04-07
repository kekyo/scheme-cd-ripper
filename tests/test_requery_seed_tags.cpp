#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "../src/cdrip/internal.h"

using cdrip::detail::append_requery_seed_tags;

namespace {

struct TestToc {
    CdRipDiscToc toc{};
    std::vector<CdRipTrackInfo> tracks{};
    std::string mb_discid{};
    std::string mb_release_id{};
    std::string mb_medium_id{};
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

auto expect_missing = [](
    const std::map<std::string, std::string>& tags,
    const std::string& key,
    const std::string& message) {

    if (tags.find(key) != tags.end()) {
        std::cerr << "assert_missing failed: " << message << "\n";
        std::cerr << "  key: " << key << "\n";
        std::exit(1);
    }
};

auto make_test_toc = [](
    const std::string& mb_discid = std::string{},
    const std::string& mb_release_id = std::string{},
    const std::string& mb_medium_id = std::string{}) {

    TestToc out{};
    out.tracks = {
        CdRipTrackInfo{1, 0, 14999, 1},
        CdRipTrackInfo{2, 15000, 26999, 1},
        CdRipTrackInfo{3, 27000, 44999, 1},
    };
    out.mb_discid = mb_discid;
    out.mb_release_id = mb_release_id;
    out.mb_medium_id = mb_medium_id;
    out.toc.leadout_sector = out.tracks.back().end + 1;
    out.toc.length_seconds = static_cast<int>(out.toc.leadout_sector / CDIO_CD_FRAMES_PER_SEC);
    return out;
};

auto sync_test_toc = [](TestToc& toc) {
    toc.toc.tracks = toc.tracks.data();
    toc.toc.tracks_count = toc.tracks.size();
    toc.toc.mb_discid = toc.mb_discid.empty() ? nullptr : toc.mb_discid.c_str();
    toc.toc.mb_release_id = toc.mb_release_id.empty() ? nullptr : toc.mb_release_id.c_str();
    toc.toc.mb_medium_id = toc.mb_medium_id.empty() ? nullptr : toc.mb_medium_id.c_str();
};

auto test_cddb_seed_tags_include_musicbrainz_requery_fields = []() {
    auto toc = make_test_toc("mb-discid-cddb");
    sync_test_toc(toc);
    std::map<std::string, std::string> tags{
        {"TITLE", "Track 2"},
    };

    append_requery_seed_tags(tags, &toc.toc, "deadbeef", 2, 3);

    expect_eq("2", tags["TRACKNUMBER"], "track number should be preserved for updates");
    expect_eq("3", tags["TRACKTOTAL"], "track total should be preserved for updates");
    expect_eq("deadbeef", tags["CDDB_DISCID"], "CDDB disc id should be embedded");
    expect_eq("0,15000,27000", tags["CDDB_OFFSETS"], "CDDB offsets should be embedded");
    expect_eq("600", tags["CDDB_TOTAL_SECONDS"], "CDDB total seconds should be embedded");
    expect_eq("mb-discid-cddb", tags["MUSICBRAINZ_DISCID"], "MusicBrainz disc id should be embedded");
    expect_eq("45150", tags["MUSICBRAINZ_LEADOUT"], "MusicBrainz leadout should be embedded");
    expect_missing(tags, "MUSICBRAINZ_RELEASE", "CDDB path should not invent MusicBrainz release ids");
    expect_missing(tags, "MUSICBRAINZ_MEDIUM", "CDDB path should not invent MusicBrainz medium ids");
};

auto test_musicbrainz_seed_tags_include_cached_release_ids = []() {
    auto toc = make_test_toc("mb-discid-release", "release-123", "medium-456");
    sync_test_toc(toc);
    std::map<std::string, std::string> tags{
        {"TITLE", "Track 1"},
    };

    append_requery_seed_tags(tags, &toc.toc, "feedcafe", 1, 3);

    expect_eq("1", tags["TRACKNUMBER"], "track number should be preserved");
    expect_eq("3", tags["TRACKTOTAL"], "track total should be preserved");
    expect_eq("feedcafe", tags["CDDB_DISCID"], "CDDB disc id should remain available");
    expect_eq("mb-discid-release", tags["MUSICBRAINZ_DISCID"], "MusicBrainz disc id should be embedded");
    expect_eq("45150", tags["MUSICBRAINZ_LEADOUT"], "MusicBrainz leadout should be embedded");
    expect_eq("release-123", tags["MUSICBRAINZ_RELEASE"], "MusicBrainz release id should be embedded");
    expect_eq("medium-456", tags["MUSICBRAINZ_MEDIUM"], "MusicBrainz medium id should be embedded");
};

auto test_musicbrainz_release_ids_are_not_overwritten = []() {
    auto toc = make_test_toc("mb-discid-release", "toc-release", "toc-medium");
    sync_test_toc(toc);
    std::map<std::string, std::string> tags{
        {"MUSICBRAINZ_RELEASE", "entry-release"},
        {"MUSICBRAINZ_MEDIUM", "entry-medium"},
    };

    append_requery_seed_tags(tags, &toc.toc, "abc12345", 3, 3);

    expect_eq("entry-release", tags["MUSICBRAINZ_RELEASE"], "existing release id should win");
    expect_eq("entry-medium", tags["MUSICBRAINZ_MEDIUM"], "existing medium id should win");
};

auto test_missing_discid_does_not_emit_partial_musicbrainz_disc_tags = []() {
    auto toc = make_test_toc("", "release-789", "medium-987");
    sync_test_toc(toc);
    std::map<std::string, std::string> tags;

    append_requery_seed_tags(tags, &toc.toc, "decafbad", 0, 3);

    expect_eq("3", tags["TRACKTOTAL"], "track total should still be embedded");
    expect_eq("release-789", tags["MUSICBRAINZ_RELEASE"], "cached release id should still be embedded");
    expect_eq("medium-987", tags["MUSICBRAINZ_MEDIUM"], "cached medium id should still be embedded");
    expect_missing(tags, "TRACKNUMBER", "track number should stay absent when unknown");
    expect_missing(tags, "MUSICBRAINZ_DISCID", "disc id should stay absent when unavailable");
    expect_missing(tags, "MUSICBRAINZ_LEADOUT", "leadout should stay absent without an exact disc id");
};

}  // namespace

int main() {
    test_cddb_seed_tags_include_musicbrainz_requery_fields();
    test_musicbrainz_seed_tags_include_cached_release_ids();
    test_musicbrainz_release_ids_are_not_overwritten();
    test_missing_discid_does_not_emit_partial_musicbrainz_disc_tags();
    return 0;
}
