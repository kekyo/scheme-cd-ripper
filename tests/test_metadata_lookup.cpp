#define main cdrip_cli_main
#include "../src/main.cpp"
#undef main

#include <cddb/cddb.h>
#include <libsoup/soup.h>
#include <functional>

struct CddbRecord {
    std::string title;
    std::vector<int> offsets;
    unsigned int seconds;
    bool readable{true};
    std::string artist{"Test Artist"};
};

static std::vector<CddbRecord> records;
static std::vector<int> query_offsets;
static unsigned int query_seconds;
static unsigned int query_id;
static size_t record_index;
static std::vector<std::string> title_queries;
static CdRipCddbServer endpoints[]{{"example.invalid", 80, "/", "test-cddb"}, {"", 80, "", "musicbrainz"}};
static CdRipCddbServerList servers{endpoints, 2};
static int failures;

static void check(bool result, const char* message) {
    if (!result) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

static void set_match(cddb_disc_t* disc) {
    cddb_disc_set_discid(disc, static_cast<unsigned int>(record_index + 1));
    cddb_disc_set_category(disc, CDDB_CAT_ROCK);
    cddb_disc_set_title(disc, records[record_index].title.c_str());
}

extern "C" int __wrap_cddb_query(cddb_conn_t*, cddb_disc_t* disc) {
    query_offsets.clear();
    for (int i = 0; i < cddb_disc_get_track_count(disc); ++i) {
        query_offsets.push_back(cddb_track_get_frame_offset(cddb_disc_get_track(disc, i)));
    }
    query_seconds = cddb_disc_get_length(disc);
    query_id = cddb_disc_get_discid(disc);
    record_index = 0;
    if (!records.empty()) set_match(disc);
    return static_cast<int>(records.size());
}

extern "C" int __wrap_cddb_query_next(cddb_conn_t*, cddb_disc_t* disc) {
    if (++record_index >= records.size()) return 0;
    set_match(disc);
    return 1;
}

extern "C" int __wrap_cddb_read(cddb_conn_t*, cddb_disc_t* disc) {
    const auto& record = records.at(cddb_disc_get_discid(disc) - 1);
    if (!record.readable || cddb_disc_get_category(disc) != CDDB_CAT_ROCK) return 0;
    cddb_disc_set_title(disc, record.title.c_str());
    cddb_disc_set_artist(disc, record.artist.c_str());
    if (record.seconds) cddb_disc_set_length(disc, record.seconds);
    for (size_t i = 0; i < record.offsets.size(); ++i) {
        auto* track = cddb_disc_get_track(disc, static_cast<int>(i));
        if (!track) {
            track = cddb_track_new();
            cddb_disc_add_track(disc, track);
        }
        if (record.offsets[i] >= 0) cddb_track_set_frame_offset(track, record.offsets[i]);
        cddb_track_set_title(track, "Song");
    }
    return 1;
}

extern "C" SoupStatus __wrap_soup_message_get_status(SoupMessage*) { return SOUP_STATUS_OK; }

extern "C" GBytes* __wrap_soup_session_send_and_read(SoupSession*, SoupMessage* msg, GCancellable*, GError**) {
    GUri* uri = soup_message_get_uri(msg);
    const std::string path = g_uri_get_path(uri);
    if (path == "/ws/2/release/") {
        char* query = g_uri_unescape_string(g_uri_get_query(uri), nullptr);
        title_queries.emplace_back(query);
        g_free(query);
    }
    const std::string body = R"({"releases":[]})";
    return g_bytes_new(body.data(), body.size());
}

// GnuDB's published 13-track query is an independent reference for the frame origin.
static void test_query_layout() {
    static const std::vector<int> offsets{
        150, 15105, 26335, 40545, 48890, 66822, 92035, 104685, 114340, 130040, 146350, 165575, 171530};
    constexpr int leadout = 2358 * 75;
    cdrip::detail::DriveBackend backend{};
    backend.get_track_count = [](void*, int& count, std::string&) {
        count = static_cast<int>(offsets.size()); return true;
    };
    backend.get_track_info = [](void*, int n, CdRipTrackInfo& track, std::string&) {
        const int next = n < static_cast<int>(offsets.size()) ? offsets[n] : leadout;
        track = {n, offsets[n - 1] - 150, next - 151, 1}; return true;
    };
    backend.get_disc_last_sector = [](void*, long& last, std::string&) {
        last = leadout - 151; return true;
    };
    CdRip drive{};
    drive.backend = &backend;
    auto* toc = cdrip_build_disc_toc(&drive, nullptr);
    check(view_string(toc->cddb_discid) == "9a09340d", "drive TOC produces GnuDB's reference ID");
    CdRipDiscToc old_tags = *toc;
    old_tags.cddb_discid = "bf09340d";
    records.clear();
    auto* entries = cdrip_fetch_cddb_entries(&old_tags, &servers, true, 2, false, nullptr, nullptr, nullptr, nullptr);
    check(query_offsets == offsets && query_seconds == 2358 && query_id == 0x9a09340d,
        "query uses the reference layout and ID, even for a previously tagged disc");
    cdrip_release_cddbentry_list(entries);
    cdrip_release_disctoc(toc);
}

static void test_cddb_candidates(const CdRipDiscToc& toc) {
    records = {
        {"Read Failure", {150, 15150}, 362, false},
        {"Wrong Length", {150, 9150}, 362},
        {"Missing Length", {150, 15150}, 0},
        {"Wrong Track Count", {150}, 362},
        {"Partial Mismatch", {150, 9000}, 0},
        {"Matching Album", {150, 15150}, 362},
    };
    title_queries.clear();
    auto* entries = cdrip_fetch_cddb_entries(&toc, &servers, true, 2, false, nullptr, nullptr, nullptr, nullptr);
    std::vector<std::string> titles;
    for (size_t i = 0; i < entries->count; ++i) titles.push_back(get_album_tag(&entries->entries[i], "ALBUM"));
    check(titles == std::vector<std::string>{"Missing Length", "Matching Album"},
        "manual selection includes the unknown and verified discs only");
    check(title_queries.size() == 1 && title_queries.front().find("matching album") != std::string::npos,
        "only a verified CDDB disc can seed title searches");
    EntryListCache cache;
    cache[build_metadata_cache_key(&toc)] = EntryListPtr(entries);
    auto selection = select_cddb_entry_for_toc(&toc, &servers, false, "", true, false, true, 2, false, &cache);
    check(selection.selected && get_album_tag(selection.selected, "ALBUM") == "Matching Album",
        "automatic selection skips unverified discs even on a metadata cache hit");
    cdrip_release_cddbentry_list(selection.entries);
    std::istringstream answers("\n1\n");
    auto* previous_input = std::cin.rdbuf(answers.rdbuf());
    auto by_default = select_cddb_entry_for_toc(&toc, &servers, false, "", false, false, true, 2, false, &cache);
    auto by_number = select_cddb_entry_for_toc(&toc, &servers, false, "", false, false, true, 2, false, &cache);
    std::cin.rdbuf(previous_input);
    check(by_default.selected && get_album_tag(by_default.selected, "ALBUM") == "Matching Album",
        "the default choice uses verified track lengths");
    check(by_number.selected && get_album_tag(by_number.selected, "ALBUM") == "Missing Length",
        "a manual choice may explicitly use an unverified album");
    cdrip_release_cddbentry_list(by_default.entries);
    cdrip_release_cddbentry_list(by_number.entries);
}

int main() {
    test_query_layout();
    CdRipTrackInfo tracks[]{{1, 0, 14999, 1}, {2, 15000, 26999, 1}};
    CdRipDiscToc toc{};
    toc.tracks = tracks;
    toc.tracks_count = 2;
    toc.length_seconds = 360;
    toc.leadout_sector = 27000;
    toc.cddb_discid = "01016802";
    test_cddb_candidates(toc);
    return failures ? 1 : 0;
}
