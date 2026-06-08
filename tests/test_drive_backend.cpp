#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "../src/cdrip/internal.h"

namespace {

constexpr int kChannels = 2;
constexpr int kSamplesPerSector = CDIO_CD_FRAMESIZE_RAW / (kChannels * static_cast<int>(sizeof(int16_t)));

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

auto expect_uint = [](
    unsigned int expected,
    unsigned int actual,
    const std::string& message) {

    if (expected != actual) {
        std::cerr << "assert_uint failed: " << message << "\n";
        std::cerr << "  expected: 0" << std::oct << expected << "\n";
        std::cerr << "  actual:   0" << std::oct << actual << std::dec << "\n";
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

auto expect_contains = [](
    const std::string& haystack,
    const std::string& needle,
    const std::string& message) {

    if (haystack.find(needle) == std::string::npos) {
        std::cerr << "assert_contains failed: " << message << "\n";
        std::cerr << "  expected to find: " << needle << "\n";
        std::cerr << "  actual: " << haystack << "\n";
        std::exit(1);
    }
};

auto expect_empty = [](
    const std::string& actual,
    const std::string& message) {

    if (!actual.empty()) {
        std::cerr << "assert_empty failed: " << message << "\n";
        std::cerr << "  actual: " << actual << "\n";
        std::exit(1);
    }
};

auto capture_stderr = [](
    auto&& fn) {

    std::ostringstream captured;
    auto* original = std::cerr.rdbuf(captured.rdbuf());
    fn();
    std::cerr.rdbuf(original);
    return captured.str();
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

struct FakeDriveHandle {
};

struct FakeReaderHandle {
};

struct FakeBackendState {
    std::vector<cdrip::detail::BackendDetectedDrive> detected_drives{};
    std::vector<CdRipTrackInfo> tracks{};
    long last_sector{0};
    std::vector<std::vector<int16_t>> sectors{};
    size_t next_sector_index{0};
    int open_calls{0};
    int close_calls{0};
    int set_speed_calls{0};
    int create_reader_calls{0};
    int destroy_reader_calls{0};
    int seek_calls{0};
    int read_calls{0};
    int eject_calls{0};
    bool last_speed_fast{false};
    CdRipRipModes last_reader_mode{RIP_MODES_DEFAULT};
    long last_seek_sector{-1};
    std::string last_open_device{};
    bool fail_open{false};
    bool fail_set_speed{false};
    bool fail_create_reader{false};
    bool fail_get_track_count{false};
    bool fail_get_track_info{false};
    bool fail_get_disc_last_sector{false};
    bool fail_seek{false};
    int fail_read_call{-1};
    bool fail_eject{false};
};

FakeBackendState* g_fake_backend_state = nullptr;

struct RecordedProgress {
    int track_number{0};
    int total_tracks{0};
    double percent{0.0};
    std::string title{};
    std::string track_name{};
    std::string path{};
};

std::vector<RecordedProgress>* g_recorded_progress = nullptr;

auto fake_detect_drives = []() {
    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    return g_fake_backend_state->detected_drives;
};

auto fake_open_drive = [](
    const std::string& device,
    void*& out_drive,
    std::string& err) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    if (g_fake_backend_state->fail_open) {
        err = "Fake open failure";
        out_drive = nullptr;
        return false;
    }
    err.clear();
    out_drive = new FakeDriveHandle{};
    g_fake_backend_state->open_calls++;
    g_fake_backend_state->last_open_device = device;
    return true;
};

auto fake_close_drive = [](
    void* drive) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    delete static_cast<FakeDriveHandle*>(drive);
    g_fake_backend_state->close_calls++;
};

auto fake_set_drive_speed = [](
    void* drive,
    bool speed_fast,
    std::string& err) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    expect_true(drive != nullptr, "fake drive handle should be valid");
    if (g_fake_backend_state->fail_set_speed) {
        err = "Fake speed failure";
        return false;
    }
    err.clear();
    g_fake_backend_state->set_speed_calls++;
    g_fake_backend_state->last_speed_fast = speed_fast;
    return true;
};

auto fake_create_reader = [](
    void* drive,
    CdRipRipModes mode,
    void*& out_reader,
    std::string& err) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    expect_true(drive != nullptr, "fake drive handle should be valid");
    if (g_fake_backend_state->fail_create_reader) {
        err = "Fake reader creation failure";
        out_reader = nullptr;
        return false;
    }
    err.clear();
    out_reader = new FakeReaderHandle{};
    g_fake_backend_state->create_reader_calls++;
    g_fake_backend_state->last_reader_mode = mode;
    return true;
};

auto fake_destroy_reader = [](
    void* reader) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    delete static_cast<FakeReaderHandle*>(reader);
    g_fake_backend_state->destroy_reader_calls++;
};

auto fake_eject_drive = [](
    const std::string& device,
    std::string& err) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    if (g_fake_backend_state->fail_eject) {
        err = "Fake eject failure";
        return false;
    }
    err.clear();
    expect_eq("/dev/fake-cdrom", device, "close should eject the selected fake device");
    g_fake_backend_state->eject_calls++;
    return true;
};

auto fake_get_track_count = [](
    void* drive,
    int& out_track_count,
    std::string& err) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    expect_true(drive != nullptr, "fake drive handle should be valid");
    if (g_fake_backend_state->fail_get_track_count) {
        err = "Fake track count failure";
        out_track_count = 0;
        return false;
    }
    err.clear();
    out_track_count = static_cast<int>(g_fake_backend_state->tracks.size());
    return out_track_count > 0;
};

auto fake_get_track_info = [](
    void* drive,
    int track_number,
    CdRipTrackInfo& out_track,
    std::string& err) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    expect_true(drive != nullptr, "fake drive handle should be valid");
    if (g_fake_backend_state->fail_get_track_info) {
        err = "Fake track info failure";
        out_track = CdRipTrackInfo{};
        return false;
    }
    err.clear();
    expect_true(track_number > 0, "track numbers should be one-based");
    const size_t index = static_cast<size_t>(track_number - 1);
    expect_true(index < g_fake_backend_state->tracks.size(), "requested fake track should exist");
    out_track = g_fake_backend_state->tracks[index];
    return true;
};

auto fake_get_disc_last_sector = [](
    void* drive,
    long& out_last_sector,
    std::string& err) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    expect_true(drive != nullptr, "fake drive handle should be valid");
    if (g_fake_backend_state->fail_get_disc_last_sector) {
        err = "Fake last sector failure";
        out_last_sector = 0;
        return false;
    }
    err.clear();
    out_last_sector = g_fake_backend_state->last_sector;
    return true;
};

auto fake_seek_reader = [](
    void* reader,
    long sector,
    std::string& err) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    expect_true(reader != nullptr, "fake reader handle should be valid");
    if (g_fake_backend_state->fail_seek) {
        err = "Fake seek failure";
        return false;
    }
    err.clear();
    expect_true(sector >= 0, "seek sector should be non-negative");
    g_fake_backend_state->seek_calls++;
    g_fake_backend_state->last_seek_sector = sector;
    g_fake_backend_state->next_sector_index = static_cast<size_t>(sector);
    return true;
};

auto fake_read_sector = [](
    void* reader,
    const int16_t*& out_buffer,
    std::string& err) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    expect_true(reader != nullptr, "fake reader handle should be valid");
    if (g_fake_backend_state->fail_read_call >= 0 &&
        g_fake_backend_state->read_calls == g_fake_backend_state->fail_read_call) {
        out_buffer = nullptr;
        err = "Fake read failure";
        return false;
    }
    err.clear();
    if (g_fake_backend_state->next_sector_index >= g_fake_backend_state->sectors.size()) {
        out_buffer = nullptr;
        err = "No more fake sectors available";
        return false;
    }
    out_buffer = g_fake_backend_state->sectors[g_fake_backend_state->next_sector_index].data();
    g_fake_backend_state->next_sector_index++;
    g_fake_backend_state->read_calls++;
    return true;
};

const cdrip::detail::DriveBackend kFakeDriveBackend{
    fake_detect_drives,
    fake_open_drive,
    fake_close_drive,
    fake_set_drive_speed,
    fake_create_reader,
    fake_destroy_reader,
    fake_eject_drive,
    fake_get_track_count,
    fake_get_track_info,
    fake_get_disc_last_sector,
    fake_seek_reader,
    fake_read_sector,
};

struct FakeBackendScope {
    explicit FakeBackendScope(
        FakeBackendState& state) {

        g_fake_backend_state = &state;
        cdrip::detail::set_drive_backend_for_tests(&kFakeDriveBackend);
    }

    ~FakeBackendScope() {
        cdrip::detail::reset_drive_backend_for_tests();
        g_fake_backend_state = nullptr;
    }
};

auto progress_callback = [](
    const CdRipProgressInfo* info) {

    expect_true(g_recorded_progress != nullptr, "progress capture should be installed");
    expect_true(info != nullptr, "progress callback payload should be valid");
    g_recorded_progress->push_back(RecordedProgress{
        info->track_number,
        info->total_tracks,
        info->percent,
        cdrip::detail::to_string_or_empty(info->title),
        cdrip::detail::to_string_or_empty(info->track_name),
        cdrip::detail::to_string_or_empty(info->path),
    });
};

struct ProgressCaptureScope {
    explicit ProgressCaptureScope(
        std::vector<RecordedProgress>& progress) {

        g_recorded_progress = &progress;
    }

    ~ProgressCaptureScope() {
        g_recorded_progress = nullptr;
    }
};

auto release_error = [](
    const char*& err) {

    cdrip_release_error(err);
    err = nullptr;
};

struct UmaskScope {
    mode_t previous;

    explicit UmaskScope(
        mode_t next) :
        previous(::umask(next)) {
    }

    ~UmaskScope() {
        ::umask(previous);
    }
};

auto filesystem_mode = [](
    const std::filesystem::path& path) {

    struct stat st {};
    expect_true(::stat(path.c_str(), &st) == 0, "path mode should be readable: " + path.string());
    return static_cast<unsigned int>(st.st_mode) & 0777U;
};

auto chmod_for_cleanup = [](
    const std::filesystem::path& path) {

    if (std::filesystem::exists(path)) {
        ::chmod(path.c_str(), 0700);
    }
};

auto make_backend_state = []() {
    FakeBackendState state{};
    state.detected_drives = {
        {"/dev/fake-empty", false},
        {"/dev/fake-cdrom", true},
    };
    state.tracks = {
        CdRipTrackInfo{1, 0, 149, 1},
        CdRipTrackInfo{2, 150, 159, 0},
        CdRipTrackInfo{3, 160, 309, 1},
    };
    state.last_sector = 309;
    state.sectors.resize(310);
    for (size_t sector = 0; sector < state.sectors.size(); ++sector) {
        auto& buffer = state.sectors[sector];
        buffer.resize(static_cast<size_t>(kSamplesPerSector) * kChannels);
        for (int sample = 0; sample < kSamplesPerSector; ++sample) {
            const int16_t value = static_cast<int16_t>(((static_cast<int>(sector) + sample) % 128) * 128);
            buffer[static_cast<size_t>(sample) * 2] = value;
            buffer[static_cast<size_t>(sample) * 2 + 1] = static_cast<int16_t>(-value);
        }
    }
    return state;
};

auto make_test_entry = []() {
    static CdRipTagKV album_tags[] = {
        CdRipTagKV{"ARTIST", "Fake Artist"},
        CdRipTagKV{"ALBUM", "Fake Album"},
        CdRipTagKV{"DATE", "2026"},
    };
    static CdRipTagKV track1_tags[] = {
        CdRipTagKV{"TITLE", "Fake Track 1"},
    };
    static CdRipTagKV track2_tags[] = {
        CdRipTagKV{"TITLE", "Fake Track 2"},
    };
    static CdRipTrackTags tracks[] = {
        CdRipTrackTags{track1_tags, 1},
        CdRipTrackTags{track2_tags, 1},
    };

    CdRipCddbEntry entry{};
    entry.cddb_discid = "feedbeef";
    entry.source_label = "fake";
    entry.source_url = "test://fake";
    entry.fetched_at = "2026-04-07T00:00:00+09:00";
    entry.album_tags = album_tags;
    entry.album_tags_count = sizeof(album_tags) / sizeof(album_tags[0]);
    entry.tracks = tracks;
    entry.tracks_count = sizeof(tracks) / sizeof(tracks[0]);
    return entry;
};

auto open_fake_rip = [](
    const CdRipSettings& settings,
    const std::string& device = "/dev/fake-cdrom") {

    const char* err = nullptr;
    CdRip* rip = cdrip_open(device.c_str(), &settings, &err);
    expect_true(rip != nullptr, err ? err : "fake drive should open");
    expect_true(err == nullptr, "open should not report an error");
    return rip;
};

auto rip_fake_track_to_path = [](
    const std::string& flac_path,
    const cdrip::detail::OutputPermissions* output_permissions) {

    auto state = make_backend_state();
    FakeBackendScope scope(state);

    const CdRipSettings settings{
        "",
        1,
        RIP_MODES_FAST,
        false,
    };
    const char* err = nullptr;
    CdRip* rip = open_fake_rip(settings);
    CdRipDiscToc* toc = cdrip_build_disc_toc(rip, &err);
    expect_true(toc != nullptr, err ? err : "fake TOC should build for permissions test");
    release_error(err);

    const auto entry = make_test_entry();
    cdrip::detail::RipTrackWriteOptions options{};
    options.output_path = flac_path.c_str();
    options.display_path = flac_path.c_str();
    options.output_permissions = output_permissions;
    std::string rip_err;
    expect_true(
        cdrip::detail::rip_track_with_options(
            rip,
            &toc->tracks[0],
            &entry,
            toc,
            nullptr,
            static_cast<int>(toc->tracks_count),
            0.0,
            0.0,
            0.0,
            &options,
            nullptr,
            rip_err),
        rip_err.empty() ? "fake rip should succeed for permissions test" : rip_err);

    cdrip_release_disctoc(toc);
    cdrip_close(rip, false, &err);
    release_error(err);
};

auto test_detect_cd_drives_uses_swapped_backend = []() {
    auto state = make_backend_state();
    FakeBackendScope scope(state);

    CdRipDetectedDriveList* list = cdrip_detect_cd_drives();
    expect_true(list != nullptr, "drive detection should return a list object");
    expect_size(2, list->count, "fake backend should control detected drive count");
    expect_eq("/dev/fake-empty", cdrip::detail::to_string_or_empty(list->drives[0].device), "first fake drive should be preserved");
    expect_true(list->drives[0].has_media == 0, "first fake drive should report no media");
    expect_eq("/dev/fake-cdrom", cdrip::detail::to_string_or_empty(list->drives[1].device), "second fake drive should be preserved");
    expect_true(list->drives[1].has_media != 0, "second fake drive should report media");
    cdrip_release_detecteddrive_list(list);
};

auto test_open_build_toc_rip_and_close_use_swapped_backend = []() {
    auto state = make_backend_state();
    FakeBackendScope scope(state);

    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-drive-backend";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    const auto flac_path = (temp_dir / "fake-track.flac").string();

    const CdRipSettings settings{
        "",
        1,
        RIP_MODES_FAST,
        true,
    };
    const char* err = nullptr;
    CdRip* rip = open_fake_rip(settings);
    expect_size(1, static_cast<size_t>(state.open_calls), "open should use the fake backend");
    expect_size(1, static_cast<size_t>(state.create_reader_calls), "reader creation should use the fake backend");
    expect_size(1, static_cast<size_t>(state.set_speed_calls), "open should request drive speed once");
    expect_true(state.last_speed_fast, "open should request fast speed from settings");
    expect_true(state.last_reader_mode == RIP_MODES_FAST, "reader creation should receive the configured mode");
    expect_eq("/dev/fake-cdrom", state.last_open_device, "open should target the selected fake device");

    CdRipDiscToc* toc = cdrip_build_disc_toc(rip, &err);
    expect_true(toc != nullptr, err ? err : "fake TOC should build");
    expect_true(err == nullptr, "TOC build should not report an error");
    expect_size(2, toc->tracks_count, "TOC should keep only audio tracks");
    expect_true(toc->tracks[0].number == 1, "first audio track should be renumbered to 1");
    expect_true(toc->tracks[1].number == 2, "second audio track should be renumbered to 2");
    expect_true(toc->tracks[0].start == 0 && toc->tracks[0].end == 149, "first audio track sectors should be preserved");
    expect_true(toc->tracks[1].start == 160 && toc->tracks[1].end == 309, "second audio track sectors should be preserved");
    expect_true(toc->length_seconds == 4, "leadout should determine the disc length");
    expect_true(cdrip::detail::to_string_or_empty(toc->cddb_discid).size() > 0, "CDDB disc id should still be computed");
    expect_true(cdrip::detail::to_string_or_empty(toc->mb_discid).size() > 0, "MusicBrainz disc id should still be computed");

    const auto entry = make_test_entry();
    cdrip::detail::RipTrackWriteOptions options{};
    options.output_path = flac_path.c_str();
    options.display_path = flac_path.c_str();
    std::string rip_err;
    expect_true(
        cdrip::detail::rip_track_with_options(
            rip,
            &toc->tracks[0],
            &entry,
            toc,
            nullptr,
            static_cast<int>(toc->tracks_count),
            0.0,
            0.0,
            0.0,
            &options,
            nullptr,
            rip_err),
        rip_err.empty() ? "fake rip should succeed" : rip_err);

    expect_true(std::filesystem::exists(flac_path), "fake rip should produce a FLAC file");
    const auto tags = read_vorbis_comments(flac_path);
    expect_eq("Fake Artist", tags.at("ARTIST"), "ripped FLAC should contain album artist tags");
    expect_eq("Fake Album", tags.at("ALBUM"), "ripped FLAC should contain album tags");
    expect_eq("Fake Track 1", tags.at("TITLE"), "ripped FLAC should contain track tags");
    expect_eq("1", tags.at("TRACKNUMBER"), "ripped FLAC should preserve the audio track number");
    expect_missing(tags, "YEAR", "ripped FLAC should not persist the format-only YEAR tag");
    expect_size(1, static_cast<size_t>(state.seek_calls), "rip should seek via the fake reader");
    expect_true(state.last_seek_sector == 0, "rip should seek to the start of the selected track");
    expect_size(150, static_cast<size_t>(state.read_calls), "rip should read the expected number of fake sectors");
    expect_size(2, static_cast<size_t>(state.set_speed_calls), "rip should request drive speed again before reading");

    cdrip_close(rip, true, &err);
    expect_true(err == nullptr, "close should not report an error");
    expect_size(1, static_cast<size_t>(state.destroy_reader_calls), "close should destroy the fake reader");
    expect_size(1, static_cast<size_t>(state.close_calls), "close should release the fake drive");
    expect_size(1, static_cast<size_t>(state.eject_calls), "close should eject through the fake backend");

    cdrip_release_disctoc(toc);
    std::filesystem::remove_all(temp_dir);
};

auto build_tags_for_date = [](
    const std::string& date) {

    CdRipTrackInfo track{1, 0, 149, 1};
    CdRipDiscToc toc{};
    toc.cddb_discid = "feedbeef";
    toc.tracks = &track;
    toc.tracks_count = 1;
    toc.leadout_sector = 150;
    toc.length_seconds = 2;

    CdRipTagKV album_tags[] = {
        CdRipTagKV{"ARTIST", "Fake Artist"},
        CdRipTagKV{"ALBUM", "Fake Album"},
        CdRipTagKV{"DATE", date.c_str()},
    };
    CdRipTagKV track_tags[] = {
        CdRipTagKV{"TITLE", "Fake Track"},
    };
    CdRipTrackTags tracks[] = {
        CdRipTrackTags{track_tags, 1},
    };
    CdRipCddbEntry entry{};
    entry.cddb_discid = "feedbeef";
    entry.album_tags = album_tags;
    entry.album_tags_count = sizeof(album_tags) / sizeof(album_tags[0]);
    entry.tracks = tracks;
    entry.tracks_count = sizeof(tracks) / sizeof(tracks[0]);

    std::string title;
    std::string track_name;
    std::string safe_title;
    return cdrip::detail::build_track_vorbis_tags(
        &track,
        &entry,
        &toc,
        1,
        nullptr,
        title,
        track_name,
        safe_title);
};

auto resolve_output_path = [](
    const std::string& format,
    const std::map<std::string, std::string>& tags) {

    std::string path;
    std::string err;
    expect_true(
        cdrip::detail::resolve_track_output_path(format, tags, path, err),
        err.empty() ? "output path should resolve" : err);
    return path;
};

auto test_year_format_tag_is_derived_from_single_valid_date_token = []() {
    const std::vector<std::string> dates = {
        "2026",
        "2026-05",
        "2026-05-24",
        "05/2026",
        "13/2026",
        "2026/05",
        "05-2026",
        "2026/05/24",
        "May 2026",
        "20th Anniversary 2026",
        "2026 remaster",
        "ca. 2026",
    };

    for (const auto& date : dates) {
        const auto tags = build_tags_for_date(date);
        expect_eq("2026", tags.at("YEAR"), "single valid year token should derive YEAR from DATE: " + date);
        expect_eq("2026.flac", resolve_output_path("{year}.flac", tags), "{year} should use the derived year: " + date);
    }
};

auto test_year_format_tag_falls_back_to_date_when_derivation_is_ambiguous_or_out_of_range = []() {
    {
        const auto tags = build_tags_for_date("1999/2000");
        expect_missing(tags, "YEAR", "multiple valid year tokens should not derive YEAR");
        expect_eq(
            "1999_2000.flac",
            resolve_output_path("{year:n}.flac", tags),
            "{year:n} should safely format DATE fallback when YEAR is ambiguous");
    }

    {
        const auto tags = build_tags_for_date("1899");
        expect_missing(tags, "YEAR", "years below 1900 should not derive YEAR");
        expect_eq("1899.flac", resolve_output_path("{year:n}.flac", tags), "{year:n} should fall back to DATE below range");
    }

    {
        const auto tags = build_tags_for_date("2101");
        expect_missing(tags, "YEAR", "years above 2100 should not derive YEAR");
        expect_eq("2101.flac", resolve_output_path("{year:n}.flac", tags), "{year:n} should fall back to DATE above range");
    }
};

auto test_tag_overrides_affect_output_path_and_vorbis_comments = []() {
    auto state = make_backend_state();
    FakeBackendScope scope(state);

    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-drive-backend-tag-overrides";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    const auto format =
        (temp_dir / "{artist:n}" / "{albumartist:n}" / "{tracknumber:02d}_{title:n}.flac").string();
    const auto expected_path =
        (temp_dir / "The Billy Bob Trio" / "The Billy Bob Trio" / "01_Fake Track 1.flac").string();

    const CdRipSettings settings{
        format.c_str(),
        1,
        RIP_MODES_FAST,
        false,
    };
    const char* err = nullptr;
    CdRip* rip = open_fake_rip(settings);
    CdRipDiscToc* toc = cdrip_build_disc_toc(rip, &err);
    expect_true(toc != nullptr, err ? err : "fake TOC should build for tag override test");
    release_error(err);

    const auto entry = make_test_entry();
    const std::map<std::string, std::string> tag_overrides{
        {"artist", "The Billy Bob Trio"},
        {"albumartist", "The Billy Bob Trio"},
    };
    cdrip::detail::RipTrackWriteOptions options{};
    options.tag_overrides = &tag_overrides;
    std::string rip_err;
    expect_true(
        cdrip::detail::rip_track_with_options(
            rip,
            &toc->tracks[0],
            &entry,
            toc,
            nullptr,
            static_cast<int>(toc->tracks_count),
            0.0,
            0.0,
            0.0,
            &options,
            nullptr,
            rip_err),
        rip_err.empty() ? "fake rip with tag overrides should succeed" : rip_err);

    expect_true(std::filesystem::exists(expected_path), "tag overrides should affect the resolved output path");
    const auto tags = read_vorbis_comments(expected_path);
    expect_eq("The Billy Bob Trio", tags.at("ARTIST"), "ARTIST should be overridden in Vorbis comments");
    expect_eq("The Billy Bob Trio", tags.at("ALBUMARTIST"), "ALBUMARTIST should be written from tag overrides");
    expect_eq("Fake Album", tags.at("ALBUM"), "unrelated tags should remain from metadata");

    cdrip_release_disctoc(toc);
    cdrip_close(rip, false, &err);
    release_error(err);
    std::filesystem::remove_all(temp_dir);
};

auto test_output_permissions_are_derived_from_umask_and_explicit_modes = []() {
    {
        UmaskScope scope(0022);
        const auto permissions = cdrip::detail::default_output_permissions_from_umask();
        expect_uint(0644, permissions.file_mode, "umask 0022 should derive file mode 0644");
        expect_uint(0755, permissions.dir_mode, "umask 0022 should derive directory mode 0755");
    }
    {
        UmaskScope scope(0002);
        const auto permissions = cdrip::detail::default_output_permissions_from_umask();
        expect_uint(0664, permissions.file_mode, "umask 0002 should derive file mode 0664");
        expect_uint(0775, permissions.dir_mode, "umask 0002 should derive directory mode 0775");
    }

    const std::vector<std::pair<unsigned int, unsigned int>> expected_dirs = {
        {0664, 0775},
        {0600, 0700},
        {0640, 0750},
        {0444, 0555},
        {0000, 0000},
    };
    for (const auto& [file_mode, dir_mode] : expected_dirs) {
        const auto permissions = cdrip::detail::output_permissions_from_file_mode(file_mode);
        expect_uint(file_mode, permissions.file_mode, "explicit file mode should be preserved");
        expect_uint(dir_mode, permissions.dir_mode, "explicit mode should derive directory execute bits");
    }
};

auto test_output_permission_warning_resolution_and_emission = []() {
    {
        const bool cli_warning = false;
        const auto permissions = cdrip::detail::resolve_output_permissions(
            0600,
            true,
            nullptr,
            &cli_warning);
        expect_uint(0600, permissions.file_mode, "config file mode should be used without CLI permissions");
        expect_uint(0700, permissions.dir_mode, "config file mode should derive directory mode");
        expect_true(!permissions.warn_on_failure, "CLI warning flag should override config true");
    }
    {
        const unsigned int cli_file_mode = 0664;
        const bool cli_warning = true;
        const auto permissions = cdrip::detail::resolve_output_permissions(
            0600,
            false,
            &cli_file_mode,
            &cli_warning);
        expect_uint(0664, permissions.file_mode, "CLI permissions should override config permissions");
        expect_uint(0775, permissions.dir_mode, "CLI permissions should derive directory mode");
        expect_true(permissions.warn_on_failure, "CLI warning flag should override config false");
    }

    const auto visible = capture_stderr([]() {
        cdrip::detail::emit_output_permissions_warning(
            "/tmp/cdrip-test.flac",
            "simulated failure",
            true);
    });
    expect_contains(
        visible,
        "Warning: failed to set permissions for /tmp/cdrip-test.flac: simulated failure",
        "warning helper should emit stderr when enabled");

    const auto hidden = capture_stderr([]() {
        cdrip::detail::emit_output_permissions_warning(
            "/tmp/cdrip-test.flac",
            "simulated failure",
            false);
    });
    expect_empty(hidden, "warning helper should suppress stderr when disabled");

    const auto empty_warning = capture_stderr([]() {
        cdrip::detail::emit_output_permissions_warning(
            "/tmp/cdrip-test.flac",
            "",
            true);
    });
    expect_empty(empty_warning, "warning helper should suppress empty warning text");
};

auto test_rip_track_applies_default_output_permissions_from_umask = []() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-drive-backend-default-permissions";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    {
        UmaskScope scope(0022);
        const auto flac_path = temp_dir / "umask-022" / "track.flac";
        rip_fake_track_to_path(flac_path.string(), nullptr);
        expect_uint(0644, filesystem_mode(flac_path), "default file mode should follow umask 0022");
        expect_uint(0755, filesystem_mode(flac_path.parent_path()), "default directory mode should follow umask 0022");
        chmod_for_cleanup(flac_path.parent_path());
    }

    {
        UmaskScope scope(0002);
        const auto flac_path = temp_dir / "umask-002" / "track.flac";
        rip_fake_track_to_path(flac_path.string(), nullptr);
        expect_uint(0664, filesystem_mode(flac_path), "default file mode should follow umask 0002");
        expect_uint(0775, filesystem_mode(flac_path.parent_path()), "default directory mode should follow umask 0002");
        chmod_for_cleanup(flac_path.parent_path());
    }

    std::filesystem::remove_all(temp_dir);
};

auto test_rip_track_applies_explicit_output_permissions = []() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-drive-backend-explicit-permissions";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto permissions = cdrip::detail::output_permissions_from_file_mode(0664);
    const auto flac_path = temp_dir / "explicit" / "track.flac";
    {
        UmaskScope scope(0077);
        rip_fake_track_to_path(flac_path.string(), &permissions);
    }

    expect_uint(0664, filesystem_mode(flac_path), "explicit file mode should override umask");
    expect_uint(0775, filesystem_mode(flac_path.parent_path()), "explicit directory mode should be derived from file mode");
    chmod_for_cleanup(flac_path.parent_path());
    std::filesystem::remove_all(temp_dir);
};

auto test_config_permissions_are_parsed = []() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-drive-backend-config-permissions";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto valid_config = temp_dir / "valid.conf";
    {
        std::ofstream out(valid_config);
        out << "[cdrip]\npermissions=640\n";
    }

    const char* err = nullptr;
    CdRipConfig* cfg = cdrip_load_config(valid_config.c_str(), &err);
    expect_true(cfg != nullptr, err ? err : "valid permissions config should load");
    expect_uint(0640, static_cast<unsigned int>(cfg->permissions), "config permissions should parse as octal");
    expect_true(cfg->permission_warnings, "permission_warnings should default to true");
    cdrip_release_config(cfg);
    release_error(err);

    const auto warnings_false_config = temp_dir / "warnings-false.conf";
    {
        std::ofstream out(warnings_false_config);
        out << "[cdrip]\npermission_warnings=false\n";
    }

    cfg = cdrip_load_config(warnings_false_config.c_str(), &err);
    expect_true(cfg != nullptr, err ? err : "permission_warnings=false config should load");
    expect_true(!cfg->permission_warnings, "permission_warnings=false should be parsed");
    cdrip_release_config(cfg);
    release_error(err);

    const auto warnings_true_config = temp_dir / "warnings-true.conf";
    {
        std::ofstream out(warnings_true_config);
        out << "[cdrip]\npermission_warnings=1\n";
    }

    cfg = cdrip_load_config(warnings_true_config.c_str(), &err);
    expect_true(cfg != nullptr, err ? err : "permission_warnings=1 config should load");
    expect_true(cfg->permission_warnings, "permission_warnings=1 should be parsed");
    cdrip_release_config(cfg);
    release_error(err);

    const auto invalid_config = temp_dir / "invalid.conf";
    {
        std::ofstream out(invalid_config);
        out << "[cdrip]\npermissions=0664\n";
    }

    cfg = cdrip_load_config(invalid_config.c_str(), &err);
    expect_true(cfg == nullptr, "4-digit permissions config should fail");
    expect_eq("Invalid permissions value", cdrip::detail::to_string_or_empty(err), "invalid config should report permissions error");
    release_error(err);

    const auto invalid_warning_config = temp_dir / "invalid-warning.conf";
    {
        std::ofstream out(invalid_warning_config);
        out << "[cdrip]\npermission_warnings=maybe\n";
    }

    cfg = cdrip_load_config(invalid_warning_config.c_str(), &err);
    expect_true(cfg == nullptr, "invalid permission_warnings config should fail");
    expect_eq(
        "Invalid permission_warnings value",
        cdrip::detail::to_string_or_empty(err),
        "invalid permission_warnings config should report permissions warning error");
    release_error(err);

    std::filesystem::remove_all(temp_dir);
};

auto test_open_reports_backend_failure = []() {
    auto state = make_backend_state();
    state.fail_open = true;
    FakeBackendScope scope(state);

    const CdRipSettings settings{"", 1, RIP_MODES_FAST, false};
    const char* err = nullptr;
    CdRip* rip = cdrip_open("/dev/fake-cdrom", &settings, &err);
    expect_true(rip == nullptr, "open should fail when backend open fails");
    expect_eq("Fake open failure", cdrip::detail::to_string_or_empty(err), "open should propagate backend error text");
    expect_size(0, static_cast<size_t>(state.close_calls), "failed open should not close a drive that never opened");
    release_error(err);
};

auto test_open_releases_drive_when_reader_creation_fails = []() {
    auto state = make_backend_state();
    state.fail_create_reader = true;
    FakeBackendScope scope(state);

    const CdRipSettings settings{"", 1, RIP_MODES_BEST, false};
    const char* err = nullptr;
    CdRip* rip = cdrip_open("/dev/fake-cdrom", &settings, &err);
    expect_true(rip == nullptr, "open should fail when reader creation fails");
    expect_eq("Fake reader creation failure", cdrip::detail::to_string_or_empty(err), "reader creation error should propagate");
    expect_size(1, static_cast<size_t>(state.open_calls), "drive should still open before reader creation");
    expect_size(1, static_cast<size_t>(state.close_calls), "drive should be released after reader creation failure");
    expect_size(0, static_cast<size_t>(state.destroy_reader_calls), "reader destruction should not run when reader creation fails");
    release_error(err);
};

auto test_build_disc_toc_reports_backend_failure_and_no_audio = []() {
    {
        auto state = make_backend_state();
        state.fail_get_track_count = true;
        FakeBackendScope scope(state);

        const CdRipSettings settings{"", 1, RIP_MODES_FAST, false};
        CdRip* rip = open_fake_rip(settings);
        const char* err = nullptr;
        CdRipDiscToc* toc = cdrip_build_disc_toc(rip, &err);
        expect_true(toc == nullptr, "TOC build should fail when backend track count fails");
        expect_eq("Fake track count failure", cdrip::detail::to_string_or_empty(err), "track count failure should propagate");
        release_error(err);

        cdrip_close(rip, false, &err);
        expect_true(err == nullptr, "close should succeed after TOC failure");
        release_error(err);
    }

    {
        auto state = make_backend_state();
        for (auto& track : state.tracks) {
            track.is_audio = 0;
        }
        FakeBackendScope scope(state);

        const CdRipSettings settings{"", 1, RIP_MODES_FAST, false};
        CdRip* rip = open_fake_rip(settings);
        const char* err = nullptr;
        CdRipDiscToc* toc = cdrip_build_disc_toc(rip, &err);
        expect_true(toc == nullptr, "TOC build should fail when the disc contains no audio tracks");
        expect_eq("No audio tracks found on disc", cdrip::detail::to_string_or_empty(err), "no-audio validation should remain stable");
        release_error(err);

        cdrip_close(rip, false, &err);
        expect_true(err == nullptr, "close should succeed after no-audio failure");
        release_error(err);
    }
};

auto test_rip_track_reports_seek_and_read_failures = []() {
    {
        auto state = make_backend_state();
        state.fail_seek = true;
        FakeBackendScope scope(state);

        const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-drive-backend-seek-failure";
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        const auto flac_path = (temp_dir / "seek-failure.flac").string();

        const CdRipSettings settings{"", 1, RIP_MODES_FAST, false};
        CdRip* rip = open_fake_rip(settings);
        const char* err = nullptr;
        CdRipDiscToc* toc = cdrip_build_disc_toc(rip, &err);
        expect_true(toc != nullptr, err ? err : "TOC build should succeed before seek failure test");
        release_error(err);

        const auto entry = make_test_entry();
        cdrip::detail::RipTrackWriteOptions options{};
        options.output_path = flac_path.c_str();
        options.display_path = flac_path.c_str();
        std::string rip_err;
        expect_true(
            !cdrip::detail::rip_track_with_options(
                rip,
                &toc->tracks[0],
                &entry,
                toc,
                nullptr,
                static_cast<int>(toc->tracks_count),
                0.0,
                0.0,
                0.0,
                &options,
                nullptr,
                rip_err),
            "seek failure should fail the rip");
        expect_eq("Fake seek failure", rip_err, "seek failure should propagate the backend message");
        expect_true(!std::filesystem::exists(flac_path), "failed seek should not publish a FLAC file");

        cdrip_release_disctoc(toc);
        cdrip_close(rip, false, &err);
        release_error(err);
        std::filesystem::remove_all(temp_dir);
    }

    {
        auto state = make_backend_state();
        state.fail_read_call = 0;
        FakeBackendScope scope(state);

        const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-drive-backend-read-failure";
        std::filesystem::remove_all(temp_dir);
        std::filesystem::create_directories(temp_dir);
        const auto flac_path = (temp_dir / "read-failure.flac").string();

        const CdRipSettings settings{"", 1, RIP_MODES_FAST, false};
        CdRip* rip = open_fake_rip(settings);
        const char* err = nullptr;
        CdRipDiscToc* toc = cdrip_build_disc_toc(rip, &err);
        expect_true(toc != nullptr, err ? err : "TOC build should succeed before read failure test");
        release_error(err);

        const auto entry = make_test_entry();
        cdrip::detail::RipTrackWriteOptions options{};
        options.output_path = flac_path.c_str();
        options.display_path = flac_path.c_str();
        std::string rip_err;
        expect_true(
            !cdrip::detail::rip_track_with_options(
                rip,
                &toc->tracks[0],
                &entry,
                toc,
                nullptr,
                static_cast<int>(toc->tracks_count),
                0.0,
                0.0,
                0.0,
                &options,
                nullptr,
                rip_err),
            "read failure should fail the rip");
        expect_eq("Read error on track 1", rip_err, "read failure should preserve the public error message");
        expect_true(!std::filesystem::exists(flac_path), "failed read should not publish a FLAC file");

        cdrip_release_disctoc(toc);
        cdrip_close(rip, false, &err);
        release_error(err);
        std::filesystem::remove_all(temp_dir);
    }
};

auto test_close_reports_eject_failure_after_cleanup = []() {
    auto state = make_backend_state();
    state.fail_eject = true;
    FakeBackendScope scope(state);

    const CdRipSettings settings{"", 1, RIP_MODES_FAST, false};
    CdRip* rip = open_fake_rip(settings);
    const char* err = nullptr;
    cdrip_close(rip, true, &err);
    expect_eq("Fake eject failure", cdrip::detail::to_string_or_empty(err), "eject failure should propagate the backend message");
    expect_size(1, static_cast<size_t>(state.destroy_reader_calls), "reader should still be destroyed before eject failure is reported");
    expect_size(1, static_cast<size_t>(state.close_calls), "drive should still be closed before eject failure is reported");
    expect_size(0, static_cast<size_t>(state.eject_calls), "failed eject should not count as a successful eject");
    release_error(err);
};

auto test_rip_track_skips_non_audio_without_reads = []() {
    auto state = make_backend_state();
    FakeBackendScope scope(state);

    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-drive-backend-skip";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    const auto flac_path = (temp_dir / "skip.flac").string();

    const CdRipSettings settings{"", 1, RIP_MODES_FAST, false};
    CdRip* rip = open_fake_rip(settings);
    const auto entry = make_test_entry();
    CdRipDiscToc toc{};
    toc.cddb_discid = "feedbeef";
    toc.tracks = state.tracks.data();
    toc.tracks_count = state.tracks.size();
    toc.leadout_sector = state.last_sector + 1;
    toc.length_seconds = static_cast<int>(toc.leadout_sector / CDIO_CD_FRAMES_PER_SEC);

    cdrip::detail::RipTrackWriteOptions options{};
    options.output_path = flac_path.c_str();
    options.display_path = flac_path.c_str();
    std::string rip_err;
    expect_true(
        cdrip::detail::rip_track_with_options(
            rip,
            &state.tracks[1],
            &entry,
            &toc,
            nullptr,
            3,
            0.0,
            0.0,
            0.0,
            &options,
            nullptr,
            rip_err),
        rip_err.empty() ? "non-audio tracks should be skipped successfully" : rip_err);
    expect_true(!std::filesystem::exists(flac_path), "skip path should not create output");
    expect_size(0, static_cast<size_t>(state.seek_calls), "skip path should not seek the reader");
    expect_size(0, static_cast<size_t>(state.read_calls), "skip path should not read sectors");

    const char* err = nullptr;
    cdrip_close(rip, false, &err);
    release_error(err);
    std::filesystem::remove_all(temp_dir);
};

auto test_rip_track_emits_progress_updates = []() {
    auto state = make_backend_state();
    FakeBackendScope scope(state);

    const auto temp_dir = std::filesystem::temp_directory_path() / "cdrip-test-drive-backend-progress";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    const auto flac_path = (temp_dir / "progress.flac").string();

    const CdRipSettings settings{"", 1, RIP_MODES_FAST, false};
    CdRip* rip = open_fake_rip(settings);
    const char* err = nullptr;
    CdRipDiscToc* toc = cdrip_build_disc_toc(rip, &err);
    expect_true(toc != nullptr, err ? err : "TOC build should succeed before progress test");
    release_error(err);

    const auto entry = make_test_entry();
    cdrip::detail::RipTrackWriteOptions options{};
    options.output_path = flac_path.c_str();
    options.display_path = flac_path.c_str();
    std::vector<RecordedProgress> progress{};
    ProgressCaptureScope progress_scope(progress);
    std::string rip_err;
    expect_true(
        cdrip::detail::rip_track_with_options(
            rip,
            &toc->tracks[0],
            &entry,
            toc,
            progress_callback,
            static_cast<int>(toc->tracks_count),
            0.0,
            4.0,
            0.0,
            &options,
            nullptr,
            rip_err),
        rip_err.empty() ? "rip with progress should succeed" : rip_err);
    expect_true(!progress.empty(), "rip should emit at least one progress callback");
    expect_true(progress.front().percent > 0.0, "first progress callback should report in-flight progress");
    expect_true(progress.back().percent >= 100.0, "last progress callback should report completion");
    expect_true(progress.back().track_number == 1, "progress callback should preserve track number");
    expect_true(progress.back().total_tracks == static_cast<int>(toc->tracks_count), "progress callback should preserve total tracks");
    expect_eq("Fake Track 1", progress.back().track_name, "progress callback should include track name");
    expect_eq(flac_path, progress.back().path, "progress callback should include output path");

    cdrip_release_disctoc(toc);
    cdrip_close(rip, false, &err);
    release_error(err);
    std::filesystem::remove_all(temp_dir);
};

}  // namespace

int main() {
    test_detect_cd_drives_uses_swapped_backend();
    test_open_build_toc_rip_and_close_use_swapped_backend();
    test_year_format_tag_is_derived_from_single_valid_date_token();
    test_year_format_tag_falls_back_to_date_when_derivation_is_ambiguous_or_out_of_range();
    test_tag_overrides_affect_output_path_and_vorbis_comments();
    test_output_permissions_are_derived_from_umask_and_explicit_modes();
    test_output_permission_warning_resolution_and_emission();
    test_rip_track_applies_default_output_permissions_from_umask();
    test_rip_track_applies_explicit_output_permissions();
    test_config_permissions_are_parsed();
    test_open_reports_backend_failure();
    test_open_releases_drive_when_reader_creation_fails();
    test_build_disc_toc_reports_backend_failure_and_no_audio();
    test_rip_track_reports_seek_and_read_failures();
    test_close_reports_eject_failure_after_cleanup();
    test_rip_track_skips_non_audio_without_reads();
    test_rip_track_emits_progress_updates();
    return 0;
}
