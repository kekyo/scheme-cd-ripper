#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#define main cdrip_main_for_progress_display_test
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

auto expect_contains = [](
    const std::string& needle,
    const std::string& actual,
    const std::string& message) {

    if (actual.find(needle) == std::string::npos) {
        std::cerr << "assert_contains failed: " << message << "\n";
        std::cerr << "  expected substring: " << needle << "\n";
        std::cerr << "  actual:             " << actual << "\n";
        std::exit(1);
    }
};

auto expect_not_contains = [](
    const std::string& needle,
    const std::string& actual,
    const std::string& message) {

    if (actual.find(needle) != std::string::npos) {
        std::cerr << "assert_not_contains failed: " << message << "\n";
        std::cerr << "  unexpected substring: " << needle << "\n";
        std::cerr << "  actual:               " << actual << "\n";
        std::exit(1);
    }
};

auto make_snapshot = []() {
    RipProgressSnapshot snapshot{};
    snapshot.track_number = 5;
    snapshot.total_tracks = 25;
    snapshot.percent = 45.0;
    snapshot.elapsed_total_sec = 123.0;
    snapshot.total_album_sec = 456.0;
    snapshot.wall_elapsed_sec = 12.0;
    snapshot.wall_total_sec = 321.0;
    snapshot.track_name = "PLANET BLUE";
    return snapshot;
};

auto test_build_rip_progress_line_keeps_inflight_spinner_state = []() {
    const RipProgressSnapshot snapshot = make_snapshot();
    const std::string line = build_rip_progress_line(snapshot, '/', false);

    expect_true(line.rfind("/ Track  5/25 ", 0) == 0, "in-flight line should keep spinner frame");
    expect_contains("[ETA: 05:09 =========>----------]", line, "in-flight line should keep a partial progress bar");
    expect_contains("\"PLANET BLUE\"", line, "in-flight line should keep the track name");
};

auto test_build_rip_progress_line_marks_completion_with_checkmark = []() {
    const RipProgressSnapshot snapshot = make_snapshot();
    const std::string line = build_rip_progress_line(snapshot, '/', true);

    expect_true(line.rfind(u8"✓ Track  5/25 ", 0) == 0, "completed line should use a checkmark");
    expect_contains("[ETA: 00:00 ====================]", line, "completed line should show a full bar");
    expect_not_contains(">", line, "completed line should not keep the in-flight arrow head");
};

auto test_print_rip_progress_line_uses_completed_rendering = []() {
    CdRipProgressInfo info{};
    info.track_number = 5;
    info.total_tracks = 25;
    info.percent = 100.0;
    info.elapsed_total_sec = 123.0;
    info.total_album_sec = 456.0;
    info.wall_elapsed_sec = 12.0;
    info.wall_total_sec = 321.0;
    info.track_name = "PLANET BLUE";

    std::ostringstream captured;
    std::streambuf* original = std::cout.rdbuf(captured.rdbuf());
    print_rip_progress_line(info);
    std::cout.rdbuf(original);

    const std::string rendered = captured.str();
    expect_contains(u8"✓ Track  5/25 ", rendered, "completed printing should use a checkmark");
    expect_contains("[ETA: 00:00 ====================]", rendered, "completed printing should show a full bar");
    expect_true(!rendered.empty() && rendered.back() == '\n', "completed printing should terminate the line");
};

}  // namespace

int main() {
    test_build_rip_progress_line_keeps_inflight_spinner_state();
    test_build_rip_progress_line_marks_completion_with_checkmark();
    test_print_rip_progress_line_uses_completed_rendering();
    return 0;
}
