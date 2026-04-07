#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
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
};

FakeBackendState* g_fake_backend_state = nullptr;

auto fake_detect_drives = []() {
    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
    return g_fake_backend_state->detected_drives;
};

auto fake_open_drive = [](
    const std::string& device,
    void*& out_drive,
    std::string& err) {

    expect_true(g_fake_backend_state != nullptr, "fake backend state should be installed");
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
    CdRip* rip = cdrip_open("/dev/fake-cdrom", &settings, &err);
    expect_true(rip != nullptr, err ? err : "fake drive should open");
    expect_true(err == nullptr, "open should not report an error");
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

}  // namespace

int main() {
    test_detect_cd_drives_uses_swapped_backend();
    test_open_build_toc_rip_and_close_use_swapped_backend();
    return 0;
}
