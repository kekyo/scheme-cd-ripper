#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "../src/cdrip/internal.h"

namespace {

struct RecordedActivity {
    CdRipActivityPhases phase{};
    CdRipActivityStates state{};
    void* callback_state{nullptr};
    std::string source_label{};
    size_t completed_sources{0};
    size_t total_sources{0};
};

struct ActivityRecorder {
    std::vector<RecordedActivity> events{};
};

struct RecordedDiagnostic {
    CdRipDiagnosticSeverities severity{};
    void* callback_state{nullptr};
    std::string source_label{};
    std::string message{};
};

struct DiagnosticRecorder {
    std::vector<RecordedDiagnostic> events{};
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

auto activity_callback = [](
    const CdRipActivityInfo* info,
    void* state,
    void* user_data) {

    auto* recorder = static_cast<ActivityRecorder*>(user_data);
    expect_true(recorder != nullptr, "observer user_data should be set");
    expect_true(info != nullptr, "observer info should be set");
    recorder->events.push_back(RecordedActivity{
        info->phase,
        info->state,
        state,
        cdrip::detail::to_string_or_empty(info->source_label),
        info->completed_sources,
        info->total_sources,
    });
};

auto diagnostic_callback = [](
    const CdRipDiagnosticInfo* info,
    void* state,
    void* user_data) {

    auto* recorder = static_cast<DiagnosticRecorder*>(user_data);
    expect_true(recorder != nullptr, "diagnostic user_data should be set");
    expect_true(info != nullptr, "diagnostic info should be set");
    recorder->events.push_back(RecordedDiagnostic{
        info->severity,
        state,
        cdrip::detail::to_string_or_empty(info->source_label),
        cdrip::detail::to_string_or_empty(info->message),
    });
};

auto test_notify_activity_passes_through_payload = []() {
    ActivityRecorder recorder{};
    CdRipActivityObserver observer{};
    observer.callback = activity_callback;
    observer.user_data = &recorder;

    CdRipActivityInfo info{};
    info.phase = CDRIP_ACTIVITY_PHASE_METADATA_FETCH;
    info.state = CDRIP_ACTIVITY_STATE_SOURCE_FINISHED;
    info.source_label = "musicbrainz";
    info.completed_sources = 2;
    info.total_sources = 3;
    void* callback_state = reinterpret_cast<void*>(0x1234);
    cdrip::detail::notify_activity(&observer, callback_state, info);

    expect_size(1, recorder.events.size(), "observer should receive one event");
    expect_true(
        recorder.events[0].phase == CDRIP_ACTIVITY_PHASE_METADATA_FETCH,
        "phase should be preserved");
    expect_true(
        recorder.events[0].state == CDRIP_ACTIVITY_STATE_SOURCE_FINISHED,
        "state should be preserved");
    expect_true(recorder.events[0].callback_state == callback_state, "callback state should be preserved");
    expect_eq("musicbrainz", recorder.events[0].source_label, "source label should be preserved");
    expect_true(recorder.events[0].completed_sources == 2, "completed count should be preserved");
    expect_true(recorder.events[0].total_sources == 3, "total count should be preserved");
};

auto test_fetch_ex_validation_paths_keep_observer_quiet = []() {
    ActivityRecorder recorder{};
    CdRipActivityObserver observer{};
    observer.callback = activity_callback;
    observer.user_data = &recorder;
    DiagnosticRecorder diagnostic_recorder{};
    CdRipDiagnosticObserver diagnostic_observer{};
    diagnostic_observer.callback = diagnostic_callback;
    diagnostic_observer.user_data = &diagnostic_recorder;

    const char* err = nullptr;
    CdRipCddbEntryList* list = cdrip_fetch_cddb_entries_ex(
        nullptr,
        nullptr,
        false,
        2,
        false,
        &observer,
        &diagnostic_observer,
        reinterpret_cast<void*>(0x1111),
        &err);
    expect_true(list != nullptr, "metadata fetch should return an empty list object");
    expect_true(list->count == 0, "metadata fetch validation should not produce entries");
    expect_eq("Invalid TOC provided", cdrip::detail::to_string_or_empty(err), "metadata fetch should report invalid TOC");
    expect_size(0, recorder.events.size(), "validation failure should not emit activity");
    expect_size(0, diagnostic_recorder.events.size(), "validation failure should not emit diagnostics");
    cdrip_release_cddbentry_list(list);
    cdrip_release_error(err);

    err = nullptr;
    expect_true(
        cdrip_fetch_cover_art_ex(nullptr, nullptr, &observer, reinterpret_cast<void*>(0x2222), &err) == 0,
        "cover art validation should fail");
    expect_eq("Invalid entry for cover art fetch", cdrip::detail::to_string_or_empty(err), "cover art should report invalid entry");
    expect_size(0, recorder.events.size(), "cover art validation should not emit activity");
    cdrip_release_error(err);

    err = nullptr;
    expect_true(
        cdrip_fetch_discogs_cover_art_ex(nullptr, nullptr, &observer, reinterpret_cast<void*>(0x3333), &err) == 0,
        "Discogs validation should fail");
    expect_eq("Invalid entry for Discogs cover art fetch", cdrip::detail::to_string_or_empty(err), "Discogs should report invalid entry");
    expect_size(0, recorder.events.size(), "Discogs validation should not emit activity");
    cdrip_release_error(err);
};

auto test_notify_diagnostic_passes_through_payload = []() {
    DiagnosticRecorder recorder{};
    CdRipDiagnosticObserver observer{};
    observer.callback = diagnostic_callback;
    observer.user_data = &recorder;

    CdRipDiagnosticInfo info{};
    info.severity = CDRIP_DIAGNOSTIC_SEVERITY_DEBUG;
    info.source_label = "musicbrainz";
    info.message = "sample diagnostic";
    void* callback_state = reinterpret_cast<void*>(0x5678);
    cdrip::detail::notify_diagnostic(&observer, callback_state, info);

    expect_size(1, recorder.events.size(), "diagnostic observer should receive one event");
    expect_true(
        recorder.events[0].severity == CDRIP_DIAGNOSTIC_SEVERITY_DEBUG,
        "diagnostic severity should be preserved");
    expect_true(recorder.events[0].callback_state == callback_state, "diagnostic state should be preserved");
    expect_eq("musicbrainz", recorder.events[0].source_label, "diagnostic source label should be preserved");
    expect_eq("sample diagnostic", recorder.events[0].message, "diagnostic message should be preserved");
};

auto test_legacy_cover_art_wrapper_still_behaves = []() {
    const char* err = nullptr;
    expect_true(
        cdrip_fetch_cover_art(nullptr, nullptr, &err) == 0,
        "legacy cover art wrapper should preserve validation result");
    expect_eq("Invalid entry for cover art fetch", cdrip::detail::to_string_or_empty(err), "legacy wrapper should preserve error");
    cdrip_release_error(err);
};

}  // namespace

int main() {
    test_notify_activity_passes_through_payload();
    test_notify_diagnostic_passes_through_payload();
    test_fetch_ex_validation_paths_keep_observer_quiet();
    test_legacy_cover_art_wrapper_still_behaves();
    return 0;
}
