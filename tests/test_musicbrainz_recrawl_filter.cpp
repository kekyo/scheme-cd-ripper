#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/cdrip/internal.h"

using cdrip::detail::build_musicbrainz_entries_from_release_json;
using cdrip::detail::release_cddb_entries;

namespace {

struct TestToc {
    CdRipDiscToc toc{};
    std::vector<CdRipTrackInfo> tracks{};
};

auto expect_true = [](bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "assert_true failed: " << message << "\n";
        std::exit(1);
    }
};

auto expect_size = [](size_t expected, size_t actual, const std::string& message) {
    if (expected != actual) {
        std::cerr << "assert_size failed: " << message << "\n";
        std::cerr << "  expected: " << expected << "\n";
        std::cerr << "  actual:   " << actual << "\n";
        std::exit(1);
    }
};

auto make_test_toc = []() {
    TestToc out{};
    out.tracks = {
        CdRipTrackInfo{1, 0, 14999, 1},
        CdRipTrackInfo{2, 15000, 26999, 1},
        CdRipTrackInfo{3, 27000, 44999, 1},
    };
    out.toc.tracks = out.tracks.data();
    out.toc.tracks_count = out.tracks.size();
    out.toc.leadout_sector = out.tracks.back().end + 1;
    out.toc.length_seconds = static_cast<int>(out.toc.leadout_sector / CDIO_CD_FRAMES_PER_SEC);
    return out;
};

auto track_lengths_from_toc_ms = [](const CdRipDiscToc& toc) {
    std::vector<int> lengths_ms;
    lengths_ms.reserve(toc.tracks_count);
    for (size_t i = 0; i < toc.tracks_count; ++i) {
        const long frames = toc.tracks[i].end - toc.tracks[i].start + 1;
        lengths_ms.push_back(static_cast<int>(std::llround(
            (static_cast<long double>(frames) * 1000.0L) / CDIO_CD_FRAMES_PER_SEC)));
    }
    return lengths_ms;
};

auto make_release_json = [](
    const std::string& release_id,
    const std::string& release_title,
    const std::string& medium_id,
    const std::vector<int>& track_lengths_ms) {

    std::ostringstream oss;
    oss << "{"
        << "\"id\":\"" << release_id << "\","
        << "\"title\":\"" << release_title << "\","
        << "\"artist-credit\":[{\"name\":\"Test Artist\"}],"
        << "\"media\":[{"
        << "\"id\":\"" << medium_id << "\","
        << "\"position\":1,"
        << "\"track-count\":" << track_lengths_ms.size() << ","
        << "\"tracks\":[";
    for (size_t i = 0; i < track_lengths_ms.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{"
            << "\"id\":\"track-" << (i + 1) << "\","
            << "\"position\":" << (i + 1) << ","
            << "\"number\":\"" << (i + 1) << "\","
            << "\"title\":\"Track " << (i + 1) << "\","
            << "\"length\":" << track_lengths_ms[i] << ","
            << "\"recording\":{"
            << "\"id\":\"recording-" << (i + 1) << "\","
            << "\"length\":" << track_lengths_ms[i]
            << "}"
            << "}";
    }
    oss << "]"
        << "}]"
        << "}";
    return oss.str();
};

auto run_release_build = [](
    const CdRipDiscToc& toc,
    const std::string& json,
    bool strict_title_recrawl_match,
    int tolerance_percent) {

    std::vector<CdRipCddbEntry> results;
    std::string err;
    const bool ok = build_musicbrainz_entries_from_release_json(
        &toc,
        "test://musicbrainz/release",
        json,
        strict_title_recrawl_match,
        tolerance_percent,
        false,
        results,
        err);
    expect_true(ok, "release JSON should parse");
    expect_true(err.empty(), "release JSON helper should not report errors");
    return results;
};

auto test_recrawl_accepts_matching_track_lengths = []() {
    const auto test_toc = make_test_toc();
    const auto lengths_ms = track_lengths_from_toc_ms(test_toc.toc);
    auto results = run_release_build(
        test_toc.toc,
        make_release_json("release-match", "Match", "medium-match", lengths_ms),
        true,
        2);
    expect_size(1, results.size(), "strict recrawl should keep the matching release");
    release_cddb_entries(results);
};

auto test_recrawl_rejects_track_count_mismatch = []() {
    const auto test_toc = make_test_toc();
    auto lengths_ms = track_lengths_from_toc_ms(test_toc.toc);
    lengths_ms.pop_back();
    auto results = run_release_build(
        test_toc.toc,
        make_release_json("release-count", "Count mismatch", "medium-count", lengths_ms),
        true,
        2);
    expect_size(0, results.size(), "strict recrawl should reject track-count mismatches");
    release_cddb_entries(results);
};

auto test_recrawl_rejects_large_length_mismatch = []() {
    const auto test_toc = make_test_toc();
    auto lengths_ms = track_lengths_from_toc_ms(test_toc.toc);
    lengths_ms[1] += 10000;
    auto results = run_release_build(
        test_toc.toc,
        make_release_json("release-length", "Length mismatch", "medium-length", lengths_ms),
        true,
        2);
    expect_size(0, results.size(), "strict recrawl should reject large track-length mismatches");
    release_cddb_entries(results);
};

auto test_recrawl_tolerance_is_configurable = []() {
    const auto test_toc = make_test_toc();
    auto lengths_ms = track_lengths_from_toc_ms(test_toc.toc);
    lengths_ms[1] += 2500;

    auto strict_results = run_release_build(
        test_toc.toc,
        make_release_json("release-tolerance", "Tolerance", "medium-tolerance", lengths_ms),
        true,
        1);
    expect_size(0, strict_results.size(), "1 percent tolerance should reject the candidate");
    release_cddb_entries(strict_results);

    auto relaxed_results = run_release_build(
        test_toc.toc,
        make_release_json("release-tolerance", "Tolerance", "medium-tolerance", lengths_ms),
        true,
        2);
    expect_size(1, relaxed_results.size(), "2 percent tolerance should keep the candidate");
    release_cddb_entries(relaxed_results);
};

auto test_lenient_helper_requires_explicit_opt_out = []() {
    const auto test_toc = make_test_toc();
    auto lengths_ms = track_lengths_from_toc_ms(test_toc.toc);
    lengths_ms[1] += 10000;
    auto results = run_release_build(
        test_toc.toc,
        make_release_json("release-lenient", "Lenient", "medium-lenient", lengths_ms),
        false,
        2);
    expect_size(1, results.size(), "lenient helper mode should only remain available by explicit opt-out");
    release_cddb_entries(results);
};

}  // namespace

int main() {
    test_recrawl_accepts_matching_track_lengths();
    test_recrawl_rejects_track_count_mismatch();
    test_recrawl_rejects_large_length_mismatch();
    test_recrawl_tolerance_is_configurable();
    test_lenient_helper_requires_explicit_opt_out();
    return 0;
}
