#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <fcntl.h>
#include <sys/wait.h>
#include <termios.h>

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
static bool fragment_terminal_reads = false;
extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t size) {
    return __real_read(fd, buffer,
        fragment_terminal_reads && ::isatty(fd) ? std::min(size, size_t{1}) : size);
}
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

// Drive a real PTY so detection, rendering, and subsequent input are checked
// together. Replies are sent in response to commands, without timing sleeps.
static void test_terminal_detection(
    const std::string& mode, const std::string& term,
    const std::string& response, bool expected_kitty,
    bool expected_query, const std::string& multiplexer,
    bool redirected_input, bool early_input) {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    expect_true(master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0,
        "create terminal for capability detection");
    const int slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
    expect_true(slave >= 0, "open terminal slave");
    termios original{};
    expect_true(::tcgetattr(slave, &original) == 0, "capture terminal settings");
    int input_pipe[2]{};
    expect_true(::pipe(input_pipe) == 0, "create redirected input");
    expect_true(::write(input_pipe[1], "2\n", 2) == 2, "prepare redirected input");
    ::close(input_pipe[1]);
    std::cout.flush();
    std::cerr.flush();
    const pid_t child = ::fork();
    expect_true(child >= 0, "fork terminal preview driver");
    if (child == 0) {
        ::close(master);
        ::dup2(redirected_input ? input_pipe[0] : slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        ::dup2(slave, STDERR_FILENO);
        ::close(input_pipe[0]);
        ::close(slave);
        ::unsetenv("TMUX");
        ::unsetenv("STY");
        ::unsetenv("COLORTERM");
        ::unsetenv("KITTY_PID");
        ::setenv("TERM", term.c_str(), 1);
        if (!multiplexer.empty()) ::setenv(multiplexer.c_str(), "test", 1);
        fragment_terminal_reads = response == "fragmented";
        cover_art_mode = mode;
        const auto png = make_cover_png(255, 0);
        const CdRipCoverArt art{png.data(), png.size(), "image/png", 1, 1};
        for (int preview = 0; preview < 2; ++preview) {
            RenderedCoverArtAscii rendered;
            std::string error;
            expect_true(render_cover_art_ascii(art, 4, rendered, error),
                "detected capability must render: " + error);
            expect_true(!rendered.graphics.empty() == expected_kitty,
                "preview format for " + mode + "/" + term + "/" + response);
            if (expected_kitty) {
                expect_true(!decode_kitty_image(rendered.graphics).pixels.empty(),
                    "detected Kitty output contains image pixels");
            }
        }
        termios restored{};
        expect_true(::tcgetattr(STDOUT_FILENO, &restored) == 0 &&
            restored.c_iflag == original.c_iflag && restored.c_oflag == original.c_oflag &&
            restored.c_lflag == original.c_lflag && restored.c_cflag == original.c_cflag &&
            std::memcmp(restored.c_cc, original.c_cc, sizeof(original.c_cc)) == 0,
            "restore terminal settings before prompting");
        expect_eq(term, std::getenv("TERM"), "detection must not change TERM");
        std::cout << "READY_FOR_SELECTION\n" << std::flush;
        expect_size(2, prompt_cover_art_choice_with_default(1),
            "terminal replies must not consume or contaminate user input");
        std::cout << "SELECTION_OK\n" << std::flush;
        ::_exit(0);
    }
    ::close(input_pipe[0]);
    ::close(slave);
    // A watchdog catches hangs; correctness is determined by the protocol and
    // the child's assertions, never by how quickly a reply happens to arrive.
    ::alarm(15);
    std::string output;
    bool replied = false;
    bool input_sent = redirected_input;
    while (true) {
        char bytes[1024];
        const auto count = ::read(master, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        output.append(bytes, static_cast<size_t>(count));
        const auto query = output.find("\x1b_G");
        if (!replied && query != std::string::npos &&
            output.find("\x1b[c", query) != std::string::npos) {
            replied = true;
            const auto id_start = output.find("i=", query);
            expect_true(id_start != std::string::npos, "query includes an image ID");
            const auto id_end = output.find_first_not_of("0123456789", id_start + 2);
            const auto id = output.substr(id_start + 2, id_end - id_start - 2);
            expect_true(output.find("a=q", query) != std::string::npos,
                "capability query must not display an image");
            std::string reply;
            if (response == "interrupt") {
                expect_true(::kill(child, SIGINT) == 0, "interrupt capability detection");
                continue;
            }
            if (early_input) {
                reply = response == "partial-input" ? "2" : "2\n";
                input_sent = response != "partial-input";
            }
            if (response == "da-first") reply += "\x1b[?1;2c";
            if (response == "ok" || response == "fragmented" || response == "da-first" ||
                response == "partial-input") reply += "\x1b_Gi=" + id + ";OK\x1b\\";
            if (response == "error") reply += "\x1b_Gi=" + id + ";EINVAL:unsupported\x1b\\";
            if (response == "wrong-id") reply += "\x1b_Gi=0;OK\x1b\\";
            if (response == "late") reply += "\x1b_Gi=" + id + ";O";
            if (response != "silent" && response != "late") reply += "\x1b[?1;2c";
            if (!reply.empty()) {
                expect_true(::write(master, reply.data(), reply.size()) ==
                    static_cast<ssize_t>(reply.size()), "send terminal response");
            }
        }
        if (!input_sent && output.find("READY_FOR_SELECTION") != std::string::npos) {
            const std::string selection = response == "partial-input" ? "\n" :
                response == "late" ? "K\x1b\\\x1b[?1;2c2\n" : "2\n";
            expect_true(::write(master, selection.data(), selection.size()) ==
                static_cast<ssize_t>(selection.size()), "select cover after detection");
            input_sent = true;
        }
    }
    termios final_settings{};
    expect_true(::tcgetattr(master, &final_settings) == 0 &&
        final_settings.c_lflag == original.c_lflag &&
        std::memcmp(final_settings.c_cc, original.c_cc, sizeof(original.c_cc)) == 0,
        "terminal settings survive normal exit and interruption");
    ::close(master);
    int status = 0;
    expect_true(::waitpid(child, &status, 0) == child, "wait for preview driver");
    ::alarm(0);
    if (response == "interrupt") {
        expect_true(replied && WIFSIGNALED(status) && WTERMSIG(status) == SIGINT,
            "interruption retains the original signal behavior");
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) std::cerr << output;
    expect_true(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "terminal detection driver succeeded");
    expect_true(replied == expected_query, "only unknown auto terminals are queried");
    if (replied) {
        const auto first = output.find("\x1b_G");
        expect_true(output.find("\x1b_G", first + 3) == std::string::npos,
            "capability is cached and responses are not echoed");
    }
    expect_true(output.find("SELECTION_OK") != std::string::npos,
        "cover selection remains usable");
}

static void test_terminal_detection_cases() {
    test_terminal_detection("auto", "xterm-256color", "da-first", false, true, "", false, false);
    test_terminal_detection("auto", "xterm-256color", "interrupt", false, true, "", false, false);
    test_terminal_detection("auto", "xterm-256color", "fragmented", true, true, "", false, true);
    test_terminal_detection("auto", "xterm-256color", "late", false, true, "", false, false);
    test_terminal_detection("auto", "xterm-256color", "partial-input", true, true, "", false, true);
    test_terminal_detection("auto", "xterm-256color", "ok", true, true, "", false, true);
    test_terminal_detection("auto", "xterm-256color", "da", false, true, "", false, false);
    test_terminal_detection("auto", "xterm-256color", "error", false, true, "", false, true);
    test_terminal_detection("auto", "xterm-256color", "wrong-id", false, true, "", false, false);
    test_terminal_detection("auto", "xterm-256color", "silent", false, true, "", false, true);
    test_terminal_detection("auto", "xterm-kitty", "silent", true, false, "", false, false);
    test_terminal_detection("kitty", "xterm-256color", "silent", true, false, "", false, false);
    test_terminal_detection("ascii", "xterm-kitty", "silent", false, false, "", false, false);
    test_terminal_detection("none", "xterm-256color", "silent", false, false, "", false, false);
    test_terminal_detection("auto", "xterm-256color", "silent", false, false, "TMUX", false, false);
    test_terminal_detection("auto", "xterm-kitty", "silent", false, false, "STY", false, false);
    test_terminal_detection("auto", "xterm-256color", "silent", false, false, "", true, false);
}

int main(int argc, char** argv) {
    if (argc > 1) {
        if (std::string(argv[1]) == "--detection") test_terminal_detection_cases();
        else if (std::string(argv[1]) == "--diagnostics") test_preview_diagnostics();
        else if (std::string(argv[1]) == "--preview") show_preview_fixture();
        else test_kitty_output();
        return 0;
    }
    test_terminal_detection_cases();
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
