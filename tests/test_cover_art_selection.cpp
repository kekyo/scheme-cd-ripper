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
        "\r\xE2\x9C\x93 Track  2/ 3 [ETA: 00:00 ====================]: \"Focused Track\"\n",
        output.str(),
        "progress callback should emit the spinner-prefixed line and terminate completed tracks");
};

}  // namespace

// Exercise renderer failures without altering the shared Chafa term database.
static bool return_empty_canvas = false;
extern "C" GString* __real_chafa_canvas_print(ChafaCanvas*, ChafaTermInfo*);
extern "C" GString* __wrap_chafa_canvas_print(ChafaCanvas* canvas, ChafaTermInfo* info) {
    return return_empty_canvas ? g_string_new("") : __real_chafa_canvas_print(canvas, info);
}

struct DecodedKittyImage {
    int width = 0;
    int height = 0;
    int columns = 0;
    int rows = 0;
    std::vector<uint8_t> pixels;
};

static DecodedKittyImage decode_kitty_image(const std::string& graphics) {
    DecodedKittyImage image;
    size_t offset = 0;
    bool complete = false;
    while ((offset = graphics.find("\x1b_G", offset)) != std::string::npos) {
        const auto end = graphics.find("\x1b\\", offset);
        expect_true(end != std::string::npos, "complete Kitty command");
        const auto separator = std::min(graphics.find(';', offset), end);
        std::istringstream fields(graphics.substr(offset + 3, separator - offset - 3));
        std::string field;
        while (std::getline(fields, field, ',')) {
            if (field.rfind("f=", 0) == 0) expect_size(32, std::stoi(field.substr(2)), "RGBA payload");
            if (field.rfind("s=", 0) == 0) image.width = std::stoi(field.substr(2));
            if (field.rfind("v=", 0) == 0) image.height = std::stoi(field.substr(2));
            if (field.rfind("c=", 0) == 0) image.columns = std::stoi(field.substr(2));
            if (field.rfind("r=", 0) == 0) image.rows = std::stoi(field.substr(2));
            if (field == "m=0") complete = true;
        }
        const auto payload = separator < end ? graphics.substr(separator + 1, end - separator - 1) : std::string{};
        if (!payload.empty()) {
            gsize size = 0;
            auto* decoded = g_base64_decode(payload.c_str(), &size);
            auto* canonical = g_base64_encode(decoded, size);
            expect_eq(payload, canonical, "each chunk has canonical Base64 padding bits");
            image.pixels.insert(image.pixels.end(), decoded, decoded + size);
            g_free(canonical);
            g_free(decoded);
        }
        offset = end + 2;
    }
    expect_true(complete && image.width > 0 && image.height > 0, "complete image with dimensions");
    expect_size(static_cast<size_t>(image.width) * image.height * 4, image.pixels.size(), "decoded image size");
    return image;
}

static void test_kitty_output() {
    expect_true(::isatty(STDOUT_FILENO) != 0, "graphics test requires a PTY");
    cover_art_mode = "kitty";
    RgbaImage image{};
    // Match the 8x16 cell geometry to test exact transfer without resampling.
    image.width = image.height = 64;
    for (int i = 0; i < 64 * 64; i++) image.pixels.insert(image.pixels.end(), {255, 0, 0, 255});
    for (bool pair : {false, true}) {
        RgbaImage composed = image;
        std::string error;
        auto right = image;
        for (size_t i = 0; i < right.pixels.size(); i += 4) {
            right.pixels[i] = 0;
            right.pixels[i + 2] = 255;
        }
        if (pair) expect_true(compose_rgba_side_by_side(image, right, 16, composed, error), "pair composition");
        RenderedCoverArtAscii rendered{};
        expect_true(render_rgba_ascii(composed, pair ? 18 : 8, rendered, error), "Kitty render");
        expect_true(rendered.graphics.find("\x1b_G") != std::string::npos, "Kitty protocol emitted");
        expect_true(rendered.lines.empty(), "graphics payload not split into text lines");
        const auto decoded = decode_kitty_image(rendered.graphics);
        expect_size(rendered.display_columns, decoded.columns, "display columns preserved");
        expect_size(rendered.display_rows, decoded.rows, "display rows preserved");
        expect_size(composed.width, decoded.width, "image width");
        expect_size(composed.height, decoded.height, "image height");
        expect_true(composed.pixels == decoded.pixels, "all cover pixels and the transparent gap survive transfer");
        std::ostringstream out;
        auto* previous = std::cout.rdbuf(out.rdbuf());
        print_cover_art(rendered);
        std::cout.rdbuf(previous);
        expect_true(out.str().find(rendered.graphics) != std::string::npos, "payload preserved");
        expect_true(out.str().find("\x1b[u\x1b[") != std::string::npos, "cursor restored below image");
    }
}

static std::vector<uint8_t> make_cover_png(uint8_t red, uint8_t blue) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = image.height = 1;
    image.format = PNG_FORMAT_RGBA;
    const uint8_t pixel[] = {red, 0, blue, 255};
    png_alloc_size_t size = 0;
    expect_true(png_image_write_to_memory(&image, nullptr, &size, 0, pixel, 0, nullptr), "PNG size");
    std::vector<uint8_t> bytes(size);
    expect_true(png_image_write_to_memory(&image, bytes.data(), &size, 0, pixel, 0, nullptr), "PNG encoding");
    bytes.resize(size);
    png_image_free(&image);
    return bytes;
}

static void test_preview_diagnostics() {
    cover_art_mode = "kitty";
    const CdRipCoverArt invalid{};
    CoverArtFetchAttempt left{}, right{};
    left.source = CoverArtFetchSource::CoverArtArchive;
    right.source = CoverArtFetchSource::Discogs;
    std::istringstream input("2\n");
    std::ostringstream output, error;
    with_redirected_stdio(input, output, error, [&]() {
        maybe_print_cover_art_ascii(invalid);
    });
    expect_true(!error.str().empty(), "single preview failure is reported to stderr");
    error.str("");
    with_redirected_stdio(input, output, error, [&]() {
        expect_size(2, prompt_for_cover_art_attempt_choice(left, right, 1, true), "selection survives preview failure");
    });
    expect_true(!error.str().empty(), "candidate preview failure is reported to stderr");
    expect_true(output.str().find("Cover art candidates:") != std::string::npos, "text candidates remain available");

    RgbaImage image{1, 1, {255, 0, 0, 255}};
    RenderedCoverArtAscii rendered;
    std::string reason;
    return_empty_canvas = true;
    const bool success = render_rgba_ascii(image, 1, rendered, reason);
    return_empty_canvas = false;
    expect_true(!success && !reason.empty(), "empty output includes a failure reason");

    const auto png = make_cover_png(255, 0);
    left.art = right.art = CdRipCoverArt{png.data(), png.size(), "image/png", 1, 1};
    std::istringstream retry_input("2\n");
    error.str("");
    output.str("");
    return_empty_canvas = true;
    with_redirected_stdio(retry_input, output, error, [&]() {
        maybe_print_cover_art_ascii(left.art);
        expect_size(2, prompt_for_cover_art_attempt_choice(left, right, 1, true), "selection survives empty renderer output");
    });
    return_empty_canvas = false;
    expect_true(error.str().find(reason) != std::string::npos, "renderer failure reason reaches stderr");
    expect_true(output.str().find("Cover art candidates:") != std::string::npos, "empty renderer falls back to text");

    cover_art_mode = "none";
    std::istringstream no_preview_input("1\n");
    error.str("");
    with_redirected_stdio(no_preview_input, output, error, [&]() {
        maybe_print_cover_art_ascii(invalid);
        left.art = right.art = invalid;
        expect_size(1, prompt_for_cover_art_attempt_choice(left, right, 1, true), "disabled preview still allows selection");
    });
    expect_true(error.str().empty(), "disabled preview emits no diagnostics");
}

// An interactive driver for terminal integration checks without a CD or network.
static void show_preview_fixture() {
    cover_art_mode = "kitty";
    const auto red = make_cover_png(255, 0);
    const auto blue = make_cover_png(0, 255);
    CoverArtFetchAttempt left{}, right{};
    left.source = CoverArtFetchSource::CoverArtArchive;
    right.source = CoverArtFetchSource::Discogs;
    left.art = CdRipCoverArt{red.data(), red.size(), "image/png", 1, 1};
    right.art = CdRipCoverArt{blue.data(), blue.size(), "image/png", 1, 1};
    std::cout << "Single red cover\n";
    maybe_print_cover_art_ascii(left.art);
    std::cout << "Next preview: " << std::flush;
    std::string line;
    std::getline(std::cin, line);
    const auto selected = prompt_for_cover_art_attempt_choice(left, right, 2, true);
    std::cout << "Selected cover " << selected << '\n';
    for (int i = 1; i <= 5; i++) std::cout << "Scroll line " << i << '\n';
    std::cout << "Preview complete: " << std::flush;
    std::getline(std::cin, line);
}

int main(int argc, char** argv) {
    if (argc > 1) {
        if (std::string(argv[1]) == "--diagnostics") test_preview_diagnostics();
        else if (std::string(argv[1]) == "--preview") show_preview_fixture();
        else test_kitty_output();
        return 0;
    }
    expect_true(select_cover_pixel_mode("auto", true, "xterm-kitty", false) == CHAFA_PIXEL_MODE_KITTY, "Kitty auto detection");
    expect_true(select_cover_pixel_mode("auto", true, "xterm-kitty", true) == CHAFA_PIXEL_MODE_SYMBOLS, "multiplexer fallback");
    expect_true(select_cover_pixel_mode("kitty", false, "xterm-kitty", false) == CHAFA_PIXEL_MODE_SYMBOLS, "redirect fallback");
    expect_true(select_cover_pixel_mode("ascii", true, "xterm-kitty", false) == CHAFA_PIXEL_MODE_SYMBOLS, "explicit ASCII");

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
