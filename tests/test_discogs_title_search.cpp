#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/cdrip/internal.h"

using cdrip::detail::DiscogsCoverArtLookupMode;
using cdrip::detail::discogs_cover_art_lookup_mode_for_entry;
using cdrip::detail::select_discogs_cover_art_candidates_from_release_jsons;

namespace {

struct TestToc {
    CdRipDiscToc toc{};
    std::vector<CdRipTrackInfo> tracks{};

    explicit TestToc(size_t track_count) {
        tracks.reserve(track_count);
        long start = 0;
        for (size_t i = 0; i < track_count; ++i) {
            tracks.push_back(CdRipTrackInfo{
                static_cast<int>(i + 1),
                start,
                start + 14999,
                1,
            });
            start += 15000;
        }
        toc.tracks = tracks.data();
        toc.tracks_count = tracks.size();
        toc.leadout_sector = tracks.empty() ? 0 : tracks.back().end + 1;
        toc.length_seconds = static_cast<int>(toc.leadout_sector / 75);
    }
};

struct TestEntry {
    CdRipCddbEntry entry{};
    std::string source_label_value{};
    std::vector<std::string> album_values{};
    std::vector<CdRipTagKV> album_tags{};
    std::vector<std::string> track_values{};
    std::vector<CdRipTagKV> track_kvs{};
    std::vector<CdRipTrackTags> tracks{};

    TestEntry(
        const std::string& source_label,
        const std::string& artist,
        const std::string& album,
        const std::vector<std::string>& track_titles,
        const std::string& discogs_release = {}) {

        source_label_value = source_label;
        album_values = {artist, album, discogs_release};
        album_tags.push_back(CdRipTagKV{"ARTIST", album_values[0].c_str()});
        album_tags.push_back(CdRipTagKV{"ALBUM", album_values[1].c_str()});
        if (!discogs_release.empty()) {
            album_tags.push_back(CdRipTagKV{"DISCOGS_RELEASE", album_values[2].c_str()});
        }

        track_values = track_titles;
        track_kvs.resize(track_values.size());
        tracks.resize(track_values.size());
        for (size_t i = 0; i < track_values.size(); ++i) {
            track_kvs[i] = CdRipTagKV{"TITLE", track_values[i].c_str()};
            tracks[i].tags = &track_kvs[i];
            tracks[i].tags_count = 1;
        }

        entry.source_label = source_label_value.c_str();
        entry.album_tags = album_tags.data();
        entry.album_tags_count = album_tags.size();
        entry.tracks = tracks.data();
        entry.tracks_count = tracks.size();
    }

    TestEntry(const TestEntry&) = delete;
    TestEntry& operator=(const TestEntry&) = delete;
};

auto expect_true = [](
    bool condition,
    const std::string& message) {

    if (!condition) {
        std::cerr << "assert_true failed: " << message << "\n";
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

auto json_escape = [](
    const std::string& value) {

    std::ostringstream oss;
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            oss << '\\' << ch;
        } else {
            oss << ch;
        }
    }
    return oss.str();
};

auto make_release_json = [](
    const std::string& id,
    const std::string& title,
    const std::string& artist,
    const std::string& format_name,
    const std::vector<std::string>& track_titles,
    bool has_image,
    int have_count) {

    std::ostringstream oss;
    oss << "{"
        << "\"id\":" << id << ","
        << "\"title\":\"" << json_escape(title) << "\","
        << "\"artists_sort\":\"" << json_escape(artist) << "\","
        << "\"artists\":[{\"name\":\"" << json_escape(artist) << "\"}],"
        << "\"formats\":[{\"name\":\"" << json_escape(format_name) << "\",\"qty\":\"1\"}],"
        << "\"community\":{\"have\":" << have_count << "},"
        << "\"tracklist\":[";
    for (size_t i = 0; i < track_titles.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{"
            << "\"position\":\"" << (i + 1) << "\","
            << "\"type_\":\"track\","
            << "\"title\":\"" << json_escape(track_titles[i]) << "\""
            << "}";
    }
    oss << "]";
    if (has_image) {
        oss << ",\"images\":[{\"type\":\"primary\",\"uri\":\"https://img.example/" << id << ".jpg\"}]";
    }
    oss << "}";
    return oss.str();
};

auto select_candidates = [](
    const TestEntry& entry,
    const TestToc& toc,
    const std::vector<std::string>& jsons,
    size_t max_candidates) {

    std::string err;
    auto candidates = select_discogs_cover_art_candidates_from_release_jsons(
        &entry.entry,
        &toc.toc,
        jsons,
        max_candidates,
        err);
    expect_true(err.empty(), "valid fixed JSON should not report parse errors");
    return candidates;
};

auto test_cddb_only_entry_accepts_high_confidence_discogs_release = []() {
    const std::vector<std::string> tracks = {"Opening", "Middle", "Finale"};
    TestEntry entry{"gnudb", "Example Artist", "Example Album", tracks};
    TestToc toc{tracks.size()};

    auto candidates = select_candidates(
        entry,
        toc,
        {make_release_json("101", "Example Album", "Example Artist", "CD", tracks, true, 12)},
        2);

    expect_size(1, candidates.size(), "high-confidence CDDB-only release should be accepted");
    expect_eq("101", candidates[0].release_id, "accepted candidate should keep the Discogs release id");
};

auto test_discogs_candidate_selection_keeps_top_two = []() {
    const std::vector<std::string> tracks = {"One", "Two", "Three"};
    TestEntry entry{"freedb", "Example Artist", "Example Album", tracks};
    TestToc toc{tracks.size()};

    auto candidates = select_candidates(
        entry,
        toc,
        {
            make_release_json("201", "Example Album", "Example Artist", "CD", tracks, true, 1),
            make_release_json("202", "Example Album", "Example Artist", "CD", tracks, true, 100),
            make_release_json("203", "Example Album", "Example Artist", "CD", tracks, true, 20),
        },
        2);

    expect_size(2, candidates.size(), "only the top two Discogs image candidates should remain");
    expect_eq("202", candidates[0].release_id, "highest-scoring candidate should be first");
    expect_eq("203", candidates[1].release_id, "second-highest candidate should be second");
};

auto test_discogs_candidate_selection_rejects_low_confidence_releases = []() {
    const std::vector<std::string> tracks = {"Alpha", "Beta", "Gamma"};
    TestEntry entry{"dbpoweramp", "Example Artist", "Example Album", tracks};
    TestToc toc{tracks.size()};

    auto candidates = select_candidates(
        entry,
        toc,
        {
            make_release_json("301", "Example Album", "Example Artist", "Vinyl", tracks, true, 50),
            make_release_json("302", "Example Album", "Example Artist", "CD", {"Alpha", "Beta"}, true, 50),
            make_release_json("303", "Example Album", "Example Artist", "CD", {"Other", "Names", "Only"}, true, 50),
            make_release_json("304", "Example Album", "Example Artist", "CD", tracks, false, 50),
        },
        2);

    expect_size(0, candidates.size(), "non-CD, mismatched, low-overlap, and image-less releases should be rejected");
};

auto test_discogs_release_id_lookup_is_preferred = []() {
    TestEntry musicbrainz_with_release{
        "musicbrainz",
        "Wrong Artist",
        "Wrong Album",
        {"Track 1"},
        "12345",
    };
    TestEntry musicbrainz_without_release{
        "musicbrainz",
        "Example Artist",
        "Example Album",
        {"Track 1"},
    };
    TestEntry cddb_without_release{
        "gnudb",
        "Example Artist",
        "Example Album",
        {"Track 1"},
    };

    expect_true(
        discogs_cover_art_lookup_mode_for_entry(&musicbrainz_with_release.entry) ==
            DiscogsCoverArtLookupMode::ReleaseId,
        "DISCOGS_RELEASE should select the release-id path even when other tags differ");
    expect_true(
        discogs_cover_art_lookup_mode_for_entry(&musicbrainz_without_release.entry) ==
            DiscogsCoverArtLookupMode::NotApplicable,
        "MusicBrainz entries without DISCOGS_RELEASE should not fall back to title search");
    expect_true(
        discogs_cover_art_lookup_mode_for_entry(&cddb_without_release.entry) ==
            DiscogsCoverArtLookupMode::TitleSearch,
        "CDDB entries without DISCOGS_RELEASE should use Discogs title search");
};

}  // namespace

int main() {
    test_cddb_only_entry_accepts_high_confidence_discogs_release();
    test_discogs_candidate_selection_keeps_top_two();
    test_discogs_candidate_selection_rejects_low_confidence_releases();
    test_discogs_release_id_lookup_is_preferred();
    return 0;
}
