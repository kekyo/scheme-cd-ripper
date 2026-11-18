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
static std::function<std::string(const std::string&, const std::string&)> mb_response =
    [](const std::string&, const std::string&) { return R"({"releases":[]})"; };
static std::map<std::string, int> detail_reads;
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
    std::string search;
    if (path == "/ws/2/release/") {
        char* query = g_uri_unescape_string(g_uri_get_query(uri), nullptr);
        search = query;
        title_queries.push_back(search);
        g_free(query);
    } else if (path.find("/ws/2/release/") == 0) {
        ++detail_reads[path.substr(path.rfind('/') + 1)];
    }
    const std::string body = mb_response(path, search);
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
    check(!title_queries.empty() && std::all_of(title_queries.begin(), title_queries.end(),
        [](const auto& q) { return q.find("matching album") != std::string::npos; }),
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

static std::string medium_json(const std::string& id, const std::string& format, int first_ms) {
    return R"({"id":")" + id + R"(","format":")" + format +
        R"(","track-count":2,"tracks":[{"position":1,"length":)" + std::to_string(first_ms) +
        R"(},{"position":2,"length":160000}]})";
}

static void test_release_cache(const CdRipDiscToc& toc) {
    records = {{"Orchard OST Disc 1", {150, 15150}, 362, true, "Various Artists"},
               {"Nebula Vol II Pt X", {150, 15150}, 362, true, "Various Artists"}};
    std::string hits;
    for (int i = 0; i < 10; ++i) {
        if (i) hits += ",";
        hits += R"({"id":"r)" + std::to_string(i) + R"(","media":[{"format":"CD","track-count":2}]})";
    }
    mb_response = [&](const std::string& path, const std::string&) {
        if (path.find("/ws/2/discid/") == 0) return std::string{R"({"releases":[{"id":"r0"}]})"};
        if (path == "/ws/2/release/") return R"({"releases":[)" + hits + "]}";
        return R"({"id":"rejected","media":[)" + medium_json("disc", "CD", 10000) + "]}";
    };
    detail_reads.clear();
    title_queries.clear();
    auto* list = cdrip_fetch_cddb_entries(&toc, &servers, true, 2, false, nullptr, nullptr, nullptr, nullptr);
    check(list->count == 2 && title_queries.size() == 16,
        "two verified CDDB titles with eight variants each yield no mismatched MusicBrainz releases");
    check(detail_reads.size() == 10 && std::all_of(detail_reads.begin(), detail_reads.end(),
        [](const auto& item) { return item.second == 1; }), "each of ten distinct release IDs is fetched just once");
    cdrip_release_cddbentry_list(list);

    records.resize(1);
    mb_response = [](const std::string& path, const std::string&) {
        if (path == "/ws/2/release/") return std::string{R"({"releases":[{"id":"r0"}]})"};
        return R"({"id":"r0","media":[)" + medium_json("disc1", "CD", 207000) + "," +
            medium_json("disc2", "CD", 208000) + "]}";
    };
    CdRipDiscToc tagged = toc;
    tagged.mb_release_id = "r0";
    detail_reads.clear();
    list = cdrip_fetch_cddb_entries(&tagged, &servers, true, 5, false, nullptr, nullptr, nullptr, nullptr);
    check(detail_reads["r0"] == 1 && list->count == 3,
        "a fresh lookup reuses a tagged release response and evaluates both media with the title search tolerance");
    cdrip_release_cddbentry_list(list);
}

static void test_title_search(const CdRipDiscToc& toc) {
    records = {{"Matching Album", {150, 15150}, 362, true, R"(A "B" & C\D)"}};
    mb_response = [](const std::string& path, const std::string&) -> std::string {
        if (path == "/ws/2/release/") return R"({"releases":[
            {"id":"wrong","media":[{"format":"Digital Media","track-count":2},{"format":"CD","track-count":3}]},
            {"id":"mixed","media":[{"format":"Enhanced CD","track-count":2},{"format":"Digital Media","track-count":2}]},
            {"id":"incomplete"}]})";
        if (path == "/ws/2/release/mixed") return R"({"id":"mixed","media":[)" +
            medium_json("audio", "Enhanced CD", 200000) + "," + medium_json("download", "Digital Media", 200000) + "]}";
        if (path == "/ws/2/release/incomplete") return R"({"id":"incomplete","media":[)" +
            medium_json("cd-side", "DualDisc (CD side)", 200000) + "," +
            medium_json("dvdplus-cd", "DVDplus (CD side)", 200000) + "]}";
        return R"({"releases":[]})";
    };
    title_queries.clear();
    detail_reads.clear();
    auto* list = cdrip_fetch_cddb_entries(&toc, &servers, true, 2, false, nullptr, nullptr, nullptr, nullptr);
    check(title_queries.size() == 1 && title_queries[0].find("tracksmedium:2") != std::string::npos &&
        title_queries[0].find(R"(artist:"A \"B\" & C\\D")") != std::string::npos,
        "a reliable artist is quoted literally and the disc's track count restricts the first search");
    check(detail_reads.count("wrong") == 0 && detail_reads["mixed"] == 1 && detail_reads["incomplete"] == 1,
        "summary format and count must agree on the same medium; incomplete summaries can be checked in detail");
    check(list->count == 4 && get_album_tag(&list->entries[1], "MUSICBRAINZ_MEDIUM") == "audio" &&
        get_album_tag(&list->entries[2], "MUSICBRAINZ_MEDIUM") == "cd-side" &&
        get_album_tag(&list->entries[3], "MUSICBRAINZ_MEDIUM") == "dvdplus-cd",
        "only CD-compatible media from matching multi-format releases are listed");
    cdrip_release_cddbentry_list(list);

    mb_response = [](const std::string& path, const std::string&) -> std::string {
        if (path == "/ws/2/release/") return R"({"releases":[{"id":"known"}]})";
        return R"({"id":"known","media":[)" + medium_json("cd", "CD", 200000) + "]}";
    };
    auto tagged = toc;
    tagged.mb_release_id = "known";
    title_queries.clear();
    detail_reads.clear();
    list = cdrip_fetch_cddb_entries(&tagged, &servers, true, 2, false, nullptr, nullptr, nullptr, nullptr);
    check(list->count == 2 && title_queries.size() == 1 && detail_reads["known"] == 1,
        "an already-listed artist-qualified match ends the title search without broadening it");
    cdrip_release_cddbentry_list(list);

    // A title hit under the supplied artist may still fail the length check.
    mb_response = [](const std::string& path, const std::string& query) -> std::string {
        if (path == "/ws/2/release/") return query.find("artist:") != std::string::npos
            ? R"({"releases":[{"id":"other"}]})" : R"({"releases":[{"id":"found"}]})";
        if (path == "/ws/2/release/other") return R"({"id":"other","media":[)" + medium_json("a", "CD", 10000) + "]}";
        if (path == "/ws/2/release/found") return R"({"id":"found","media":[)" + medium_json("b", "CD", 200000) + "]}";
        return R"({"releases":[]})";
    };
    title_queries.clear();
    list = cdrip_fetch_cddb_entries(&toc, &servers, true, 2, false, nullptr, nullptr, nullptr, nullptr);
    check(title_queries.size() == 2 && title_queries[0].find("artist:") != std::string::npos &&
        title_queries[1].find("artist:") == std::string::npos && title_queries[1].find("tracksmedium:2") != std::string::npos &&
        list->count == 2, "fall back to title and medium track count after artist-qualified hits fail validation");
    cdrip_release_cddbentry_list(list);

    for (const std::string artist : {"Various Artists", "Unknown Artist", ""}) {
        records.front().artist = artist;
        title_queries.clear();
        list = cdrip_fetch_cddb_entries(&toc, &servers, true, 2, false, nullptr, nullptr, nullptr, nullptr);
        check(title_queries.size() == 1 && title_queries.front().find("artist:") == std::string::npos,
            "compilation, placeholder, and missing artist names do not restrict title searches");
        cdrip_release_cddbentry_list(list);
    }
    records.front().artist = "Singer A";
    records.push_back({"Matching Album", {150, 15150}, 362, true, "Singer B"});
    title_queries.clear();
    list = cdrip_fetch_cddb_entries(&toc, &servers, true, 2, false, nullptr, nullptr, nullptr, nullptr);
    check(title_queries.size() == 1 && title_queries.front().find("artist:") == std::string::npos,
        "conflicting verified artist credits leave the shared title unrestricted");
    cdrip_release_cddbentry_list(list);
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
    test_release_cache(toc);
    test_title_search(toc);
    return failures ? 1 : 0;
}
