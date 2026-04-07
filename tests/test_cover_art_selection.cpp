#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#define main cdrip_app_main_for_tests
#include "../src/main.cpp"
#undef main

namespace {

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

auto with_redirected_stdio = [](
    std::istream& input,
    std::ostream& output,
    std::ostream& error,
    auto&& fn) {

    auto* old_in = std::cin.rdbuf(input.rdbuf());
    auto* old_out = std::cout.rdbuf(output.rdbuf());
    auto* old_err = std::cerr.rdbuf(error.rdbuf());
    fn();
    std::cin.rdbuf(old_in);
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
};

auto test_should_offer_cover_art_choice_only_when_both_sources_exist = []() {
    expect_true(
        should_offer_cover_art_choice(true, true, true),
        "interactive single-disc mode should offer a choice when both sources are available");
    expect_true(
        !should_offer_cover_art_choice(false, true, true),
        "non-interactive modes should not offer a cover art choice");
    expect_true(
        !should_offer_cover_art_choice(true, true, false),
        "single-source availability should not enter the choice UI");
    expect_true(
        !should_offer_cover_art_choice(true, false, true),
        "single-source availability should not enter the choice UI");
};

auto test_default_cover_art_choice_matches_discogs_mode = []() {
    expect_size(1, default_cover_art_choice(DiscogsMode::No), "discogs=no should default to Cover Art Archive");
    expect_size(1, default_cover_art_choice(DiscogsMode::Fallback), "discogs=fallback should default to Cover Art Archive");
    expect_size(2, default_cover_art_choice(DiscogsMode::Always), "discogs=always should default to Discogs");
};

auto test_build_cover_art_choice_header_centers_labels = []() {
    const std::string preview = build_cover_art_choice_header("[1]", "[2]", 4);
    expect_eq(
        "[1]    [2] \n"
        ,
        preview,
        "header should center labels inside each preview column");
};

auto test_compose_rgba_side_by_side_keeps_gap_transparent = []() {
    RgbaImage left{};
    left.width = 2;
    left.height = 1;
    left.pixels = {
        255, 0, 0, 255,
        0, 255, 0, 255,
    };

    RgbaImage right{};
    right.width = 1;
    right.height = 2;
    right.pixels = {
        0, 0, 255, 255,
        255, 255, 0, 255,
    };

    RgbaImage composite{};
    std::string err;
    expect_true(
        compose_rgba_side_by_side(left, right, 2, composite, err),
        "composition should succeed for valid images");
    expect_true(err.empty(), "successful composition should not report errors");
    expect_size(5, composite.width, "composite width should include left image, gap, and right image");
    expect_size(2, composite.height, "composite height should keep the taller image");

    auto pixel = [&](int x, int y, int channel) -> uint8_t {
        return composite.pixels[(static_cast<size_t>(y) * composite.width + x) * 4 + channel];
    };

    expect_true(pixel(0, 0, 0) == 255 && pixel(0, 0, 3) == 255, "left image should be copied to the composite");
    expect_true(pixel(1, 0, 1) == 255 && pixel(1, 0, 3) == 255, "left image should preserve all channels");
    expect_true(pixel(2, 0, 3) == 0 && pixel(3, 0, 3) == 0, "gap pixels should remain transparent");
    expect_true(pixel(4, 0, 2) == 255 && pixel(4, 0, 3) == 255, "right image top pixel should be copied");
    expect_true(pixel(4, 1, 0) == 255 && pixel(4, 1, 1) == 255 && pixel(4, 1, 3) == 255, "right image lower pixel should be copied");
    expect_true(pixel(0, 1, 3) == 0, "unused area under the shorter image should stay transparent");
};

auto test_prompt_for_cover_art_source_honors_default_and_retries = []() {
    CoverArtFetchAttempt left{};
    left.source = CoverArtFetchSource::CoverArtArchive;
    left.success = true;

    CoverArtFetchAttempt right{};
    right.source = CoverArtFetchSource::Discogs;
    right.success = true;

    {
        std::istringstream input("\n");
        std::ostringstream output;
        std::ostringstream error;
        CoverArtFetchSource selected = CoverArtFetchSource::None;
        with_redirected_stdio(input, output, error, [&]() {
            prompt_for_cover_art_source(left, right, DiscogsMode::Always, false, selected);
        });
        expect_true(
            selected == CoverArtFetchSource::Discogs,
            "empty input should choose Discogs when discogs=always");
    }

    {
        std::istringstream input("9\n1\n");
        std::ostringstream output;
        std::ostringstream error;
        CoverArtFetchSource selected = CoverArtFetchSource::None;
        with_redirected_stdio(input, output, error, [&]() {
            prompt_for_cover_art_source(left, right, DiscogsMode::Fallback, false, selected);
        });
        expect_true(
            selected == CoverArtFetchSource::CoverArtArchive,
            "invalid input should retry until a valid source selection is entered");
    }
};

auto test_build_rip_progress_line_adds_spinner_and_title_fallback = []() {
    CdRipProgressInfo info{};
    info.track_number = 1;
    info.total_tracks = 12;
    info.percent = 25.0;
    info.elapsed_total_sec = 30.0;
    info.total_album_sec = 120.0;
    info.wall_elapsed_sec = 0.0;
    info.wall_total_sec = 0.0;
    info.title = "Album Title";
    info.track_name = "";
    const RipProgressSnapshot snapshot = make_rip_progress_snapshot(info);

    expect_eq(
        "| Track  1/12 [ETA: --:-- =====>--------------]: \"Album Title\"",
        build_rip_progress_line(snapshot, '|'),
        "rip progress line should include an initial spinner frame and fall back to the title");
};

auto test_build_rip_progress_line_updates_spinner_and_eta_after_threshold = []() {
    CdRipProgressInfo info{};
    info.track_number = 2;
    info.total_tracks = 3;
    info.percent = 100.0;
    info.elapsed_total_sec = 45.0;
    info.total_album_sec = 120.0;
    info.wall_elapsed_sec = 10.2;
    info.wall_total_sec = 65.2;
    info.title = "Ignored Title";
    info.track_name = "Focused Track";
    const RipProgressSnapshot snapshot = make_rip_progress_snapshot(info);

    expect_eq(
        "- Track  2/ 3 [ETA: 00:55 ====================]: \"Focused Track\"",
        build_rip_progress_line(snapshot, '-'),
        "rip progress line should rotate the spinner and switch to ETA display after enough elapsed time");
};

auto test_rip_progress_callback_writes_spinner_line_and_completion_newline = []() {
    CdRipProgressInfo info{};
    info.track_number = 2;
    info.total_tracks = 3;
    info.percent = 100.0;
    info.elapsed_total_sec = 45.0;
    info.total_album_sec = 120.0;
    info.wall_elapsed_sec = 10.2;
    info.wall_total_sec = 65.2;
    info.title = "Ignored Title";
    info.track_name = "Focused Track";

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    with_redirected_stdio(input, output, error, [&]() {
        RipProgressSpinner::progress_cb(&info);
    });

    expect_eq(
        "\r- Track  2/ 3 [ETA: 00:55 ====================]: \"Focused Track\"\n",
        output.str(),
        "progress callback should emit the spinner-prefixed line and terminate completed tracks");
};

}  // namespace

int main() {
    test_should_offer_cover_art_choice_only_when_both_sources_exist();
    test_default_cover_art_choice_matches_discogs_mode();
    test_build_cover_art_choice_header_centers_labels();
    test_compose_rgba_side_by_side_keeps_gap_transparent();
    test_prompt_for_cover_art_source_honors_default_and_retries();
    test_build_rip_progress_line_adds_spinner_and_title_fallback();
    test_build_rip_progress_line_updates_spinner_and_eta_after_threshold();
    test_rip_progress_callback_writes_spinner_line_and_completion_newline();
    return 0;
}
