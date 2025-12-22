// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <unordered_set>
#include <vector>

#include <cddb/cddb.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <gio/gio.h>

#include "internal.h"
#include "http_retry.h"
#include "version.h"

using namespace cdrip::detail;

/* ------------------------------------------------------------------- */

// ## Tech info
// 
// ### Succeeded MusicBrainz API patterns
// 
// ABBA GOLD GREATEST HITS (only disc id):
// 
// ```bash
// curl -A "SchemeCDRipper-1.0 (test)" "https://musicbrainz.org/ws/2/discid/JccSw1uJ4N1gVYL6pc3GfkTluOM-?fmt=json&inc=recordings+artists"
// ```
// 
// The scheme (could not get only disc id, required toc information):
// 
// ```bash
// toc="1+23+222950+150+4588+11120+25623+31650+42885+56575+65440+73830+80250+87383+97278+105118+111388+122720+136860+138538+144740+153085+160043+177988+197713+208563"
// curl -A "SchemeCDRipper-1.0 (test)" "https://musicbrainz.org/ws/2/discid/FM.MBLY8xdyWv7S6.RUrTT6893s-?fmt=json&toc=${toc}&inc=recordings+artists"
// ```
// 
// ** When calling MusicBrainz API from cdrip, always include the `toc` parameter even if the disc ID can be uniquely retrieved.**

/* ------------------------------------------------------------------- */
/* Use static linkage for file local definitions */

constexpr int kMusicBrainzTimeoutSec = 10;
constexpr int kMusicBrainzRetryDelayMs = 1200;
constexpr int kMusicBrainzMaxAttempts = 3;
constexpr int kMusicBrainzSearchLimit = 10;

static const char* kMusicBrainzLabel = "musicbrainz";
// Includes kept minimal but must contain genres/tags so we can populate GENRE.
// DiscID lookup: cover-art-archive is invalid here; fetch cover art in a later release lookup.
static const char* kMusicBrainzInc = "recordings+artists+release-groups+genres+tags+url-rels";
// Note: cover-art-archive is not a valid inc for release lookup; cover art is fetched separately.
static const char* kMusicBrainzReleaseInc = "recordings+artists+artist-credits+media+discids+labels+release-groups+genres+tags+url-rels";

static std::string musicbrainz_user_agent() {
    std::string ua = "SchemeCDRipper/";
    ua += VERSION;
    ua += " (https://github.com/kekyo/scheme-cd-ripper)";
    return ua;
}

static std::string get_string_member(JsonObject* obj, const char* name) {
    if (!obj || !name) return {};
    if (!json_object_has_member(obj, name)) return {};
    JsonNode* node = json_object_get_member(obj, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return {};
    const gchar* value = json_object_get_string_member(obj, name);
    return value ? std::string{value} : std::string{};
}

static void release_cddb_entry(CdRipCddbEntry& e) {
    release_cstr(e.cddb_discid);
    release_cstr(e.source_label);
    release_cstr(e.source_url);
    release_cstr(e.fetched_at);
    if (e.cover_art.data) {
        delete[] e.cover_art.data;
        e.cover_art.data = nullptr;
    }
    release_cstr(e.cover_art.mime_type);
    e.cover_art.size = 0;
    e.cover_art.available = 0;
    e.cover_art.is_front = 0;
    if (e.album_tags) {
        for (size_t ti = 0; ti < e.album_tags_count; ++ti) {
            release_cstr(e.album_tags[ti].key);
            release_cstr(e.album_tags[ti].value);
        }
        delete[] e.album_tags;
        e.album_tags = nullptr;
    }
    e.album_tags_count = 0;
    if (e.tracks) {
        for (size_t t = 0; t < e.tracks_count; ++t) {
            CdRipTrackTags* tt = &e.tracks[t];
            if (tt->tags) {
                for (size_t kv = 0; kv < tt->tags_count; ++kv) {
                    release_cstr(tt->tags[kv].key);
                    release_cstr(tt->tags[kv].value);
                }
                delete[] tt->tags;
                tt->tags = nullptr;
            }
            tt->tags_count = 0;
        }
        delete[] e.tracks;
        e.tracks = nullptr;
    }
    e.tracks_count = 0;
}

static int get_int_member(JsonObject* obj, const char* name, int fallback = -1) {
    if (!obj || !name) return fallback;
    if (!json_object_has_member(obj, name)) return fallback;
    return json_object_get_int_member(obj, name);
}

static std::string build_musicbrainz_release_key(const CdRipCddbEntry& entry) {
    const std::string release = trim(album_tag(&entry, "MUSICBRAINZ_RELEASE"));
    if (release.empty()) return {};
    const std::string medium = trim(album_tag(&entry, "MUSICBRAINZ_MEDIUM"));
    if (!medium.empty()) return release + ":" + medium;
    return release;
}

static JsonArray* get_array_member(JsonObject* obj, const char* name) {
    if (!obj || !name) return nullptr;
    if (!json_object_has_member(obj, name)) return nullptr;
    return json_object_get_array_member(obj, name);
}

static JsonObject* get_object_member(JsonObject* obj, const char* name) {
    if (!obj || !name) return nullptr;
    if (!json_object_has_member(obj, name)) return nullptr;
    return json_object_get_object_member(obj, name);
}

static bool get_bool_member(JsonObject* obj, const char* name, bool fallback = false) {
    if (!obj || !name) return fallback;
    if (!json_object_has_member(obj, name)) return fallback;
    return json_object_get_boolean_member(obj, name);
}

static std::string extract_discogs_release_id_from_url(const std::string& url) {
    if (url.empty()) return {};
    const std::string lower = to_lower(url);
    static constexpr const char* kMarker = "/release/";
    const size_t pos = lower.find(kMarker);
    if (pos == std::string::npos) return {};
    size_t i = pos + std::string{kMarker}.size();
    const size_t start = i;
    while (i < url.size() && std::isdigit(static_cast<unsigned char>(url[i]))) {
        ++i;
    }
    if (i == start) return {};
    return url.substr(start, i - start);
}

static std::string extract_discogs_release_id(JsonObject* release_obj) {
    if (!release_obj) return {};
    JsonArray* relations = get_array_member(release_obj, "relations");
    if (!relations) return {};
    const guint len = json_array_get_length(relations);
    for (guint i = 0; i < len; ++i) {
        JsonObject* rel = json_array_get_object_element(relations, i);
        if (!rel) continue;
        const std::string type = to_lower(get_string_member(rel, "type"));
        if (type != "discogs") continue;
        JsonObject* url_obj = get_object_member(rel, "url");
        const std::string resource = get_string_member(url_obj, "resource");
        const std::string id = extract_discogs_release_id_from_url(resource);
        if (!id.empty()) return id;
    }
    return {};
}

static std::string join_artist_credit(JsonArray* ac) {
    if (!ac) return {};
    std::ostringstream oss;
    const guint len = json_array_get_length(ac);
    for (guint i = 0; i < len; ++i) {
        JsonObject* item = json_array_get_object_element(ac, i);
        if (!item) continue;
        std::string name = get_string_member(item, "name");
        if (name.empty()) {
            JsonObject* artist = get_object_member(item, "artist");
            name = get_string_member(artist, "name");
        }
        std::string join = get_string_member(item, "joinphrase");
        oss << name;
        if (!join.empty()) oss << join;
    }
    return trim(oss.str());
}

static std::vector<long> build_mb_offsets(
    const CdRipDiscToc* toc,
    long& out_leadout) {

    std::vector<long> offsets;
    out_leadout = 0;
    if (!toc || !toc->tracks || toc->tracks_count == 0) return offsets;
    offsets.reserve(toc->tracks_count);
    const long leadout_raw = (toc->leadout_sector > 0)
        ? toc->leadout_sector
        : (toc->tracks[toc->tracks_count - 1].end + 1);
    out_leadout = leadout_raw + 150;
    for (size_t i = 0; i < toc->tracks_count; ++i) {
        offsets.push_back(toc->tracks[i].start + 150);
    }
    return offsets;
}

static std::string build_toc_param(const CdRipDiscToc* toc) {
    long mb_leadout = 0;
    std::vector<long> offsets = build_mb_offsets(toc, mb_leadout);
    if (offsets.empty()) return {};
    const int first_track = toc->tracks[0].number;
    const int last_track = toc->tracks[toc->tracks_count - 1].number;
    std::ostringstream oss;
    oss << first_track << "+" << last_track << "+" << mb_leadout;
    for (long offset : offsets) {
        oss << "+" << offset;
    }
    return oss.str();
}

static bool offsets_match(JsonArray* arr, const std::vector<long>& expected) {
    if (!arr) return false;
    const guint len = json_array_get_length(arr);
    if (expected.empty() || len != expected.size()) return false;
    for (guint i = 0; i < len; ++i) {
        const long value = json_array_get_int_element(arr, i);
        if (value != expected[i]) return false;
    }
    return true;
}

static bool medium_matches(
    JsonObject* medium,
    const CdRipDiscToc* toc,
    const std::vector<long>& offsets,
    const std::string& discid,
    const std::string& preferred_medium) {

    if (!medium) return false;
    if (!preferred_medium.empty()) {
        const std::string mid = get_string_member(medium, "id");
        if (!mid.empty() && mid == preferred_medium) return true;
    }

    JsonArray* discs = get_array_member(medium, "discs");
    if (discs) {
        const guint len = json_array_get_length(discs);
        for (guint i = 0; i < len; ++i) {
            JsonObject* disc = json_array_get_object_element(discs, i);
            if (!disc) continue;
            const std::string did = get_string_member(disc, "id");
            if (!discid.empty() && !did.empty() && did == discid) {
                return true;
            }
            if (offsets_match(get_array_member(disc, "offsets"), offsets)) {
                return true;
            }
        }
    }

    const int track_count = get_int_member(medium, "track-count", -1);
    if (track_count > 0 && static_cast<size_t>(track_count) == toc->tracks_count) {
        return true;
    }
    return false;
}

static std::vector<JsonObject*> select_matching_media(
    JsonArray* media_array,
    const CdRipDiscToc* toc,
    const std::vector<long>& offsets,
    const std::string& discid,
    const std::string& preferred_medium) {

    std::vector<JsonObject*> matches;
    std::vector<JsonObject*> same_tracks;
    if (!media_array) return matches;
    const guint len = json_array_get_length(media_array);

    if (!discid.empty()) {
        std::vector<JsonObject*> discid_matches;
        for (guint i = 0; i < len; ++i) {
            JsonObject* medium = json_array_get_object_element(media_array, i);
            if (!medium) continue;
            JsonArray* discs = get_array_member(medium, "discs");
            if (!discs) continue;
            const guint discs_len = json_array_get_length(discs);
            for (guint di = 0; di < discs_len; ++di) {
                JsonObject* disc = json_array_get_object_element(discs, di);
                if (!disc) continue;
                const std::string did = get_string_member(disc, "id");
                if (!did.empty() && did == discid) {
                    discid_matches.push_back(medium);
                    break;
                }
            }
        }
        if (!discid_matches.empty()) return discid_matches;
    }

    for (guint i = 0; i < len; ++i) {
        JsonObject* medium = json_array_get_object_element(media_array, i);
        if (!medium) continue;
        if (medium_matches(medium, toc, offsets, discid, preferred_medium)) {
            matches.push_back(medium);
        } else {
            const int track_count = get_int_member(medium, "track-count", -1);
            if (track_count > 0 && static_cast<size_t>(track_count) == toc->tracks_count) {
                same_tracks.push_back(medium);
            }
        }
    }
    if (!matches.empty()) return matches;
    if (!same_tracks.empty()) return same_tracks;
    if (len > 0) {
        JsonObject* first_medium = json_array_get_object_element(media_array, 0);
        if (first_medium) matches.push_back(first_medium);
    }
    return matches;
}

static bool http_get_json(const std::string& url, std::string& body, std::string& err) {
    HttpRetryPolicy policy{};
    policy.timeout_sec = kMusicBrainzTimeoutSec;
    policy.max_attempts = kMusicBrainzMaxAttempts;
    policy.retry_delay_ms = kMusicBrainzRetryDelayMs;
    policy.max_redirects = 2;
    policy.respect_retry_after = true;

    std::vector<uint8_t> bytes;
    std::string ct;
    if (!http_get_bytes_with_retry(
            "MusicBrainz",
            url,
            musicbrainz_user_agent(),
            "application/json",
            policy,
            bytes,
            ct,
            err)) {
        return false;
    }

    body.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

static std::string escape_mb_query(const std::string& value) {
    gchar* escaped = g_uri_escape_string(value.c_str(), nullptr, true);
    if (!escaped) return {};
    std::string out = escaped;
    g_free(escaped);
    return out;
}

static std::string build_musicbrainz_release_search_url(const std::string& album_title) {
    std::string title = trim(album_title);
    if (title.empty()) return {};
    std::string sanitized;
    sanitized.reserve(title.size());
    for (char ch : title) {
        if (ch == '"') continue;
        sanitized.push_back(ch);
    }
    if (sanitized.empty()) return {};
    const std::string query = "release:\"" + sanitized + "\"";
    const std::string encoded = escape_mb_query(query);
    if (encoded.empty()) return {};
    std::ostringstream oss;
    oss << "https://musicbrainz.org/ws/2/release/?fmt=json&limit=" << kMusicBrainzSearchLimit
        << "&query=" << encoded;
    return oss.str();
}

static bool build_entries_from_release(
    const CdRipDiscToc* toc,
    const std::string& request_url,
    JsonObject* release_obj,
    const std::vector<long>& offsets,
    const std::string& discid,
    std::vector<CdRipCddbEntry>& results);

static bool fetch_release_details_and_build(
    const CdRipDiscToc* toc,
    const std::string& release_id,
    const std::vector<long>& offsets,
    const std::string& discid,
    std::vector<CdRipCddbEntry>& results,
    std::string& err) {

    if (release_id.empty()) return false;
    const std::string url = "https://musicbrainz.org/ws/2/release/" + release_id +
        "?fmt=json&inc=" + kMusicBrainzReleaseInc;

    std::string body;
    if (!http_get_json(url, body, err)) {
        return false;
    }

    JsonParser* parser = json_parser_new();
    GError* gerr = nullptr;
    if (!json_parser_load_from_data(parser, body.c_str(), body.size(), &gerr)) {
        err = gerr && gerr->message ? std::string{gerr->message} : "MusicBrainz release parse error";
        if (gerr) g_error_free(gerr);
        g_object_unref(parser);
        return false;
    }
    JsonNode* root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        err = "MusicBrainz release response is not a JSON object";
        g_object_unref(parser);
        return false;
    }
    JsonObject* root_obj = json_node_get_object(root);
    bool ok = build_entries_from_release(toc, url, root_obj, offsets, discid, results);
    g_object_unref(parser);
    return ok;
}

static void append_tag(
    std::vector<CdRipTagKV>& tags,
    const std::string& key,
    const std::string& value) {

    if (!value.empty()) {
        tags.push_back(make_kv(key, value));
    }
}

static std::string join_strings(const std::vector<std::string>& values, const std::string& sep) {
    if (values.empty()) return {};
    std::ostringstream oss;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) oss << sep;
        oss << values[i];
    }
    return oss.str();
}

static void append_unique(std::vector<std::string>& dest, const std::string& value) {
    if (value.empty()) return;
    if (std::find(dest.begin(), dest.end(), value) == dest.end()) {
        dest.push_back(value);
    }
}

static void collect_string_array(JsonArray* arr, std::vector<std::string>& out) {
    if (!arr) return;
    const guint len = json_array_get_length(arr);
    for (guint i = 0; i < len; ++i) {
        const gchar* v = json_array_get_string_element(arr, i);
        if (v) append_unique(out, v);
    }
}

static void collect_name_from_object_array(
    JsonArray* arr,
    const char* name_key,
    std::vector<std::string>& out) {

    if (!arr || !name_key) return;
    const guint len = json_array_get_length(arr);
    for (guint i = 0; i < len; ++i) {
        JsonObject* obj = json_array_get_object_element(arr, i);
        append_unique(out, get_string_member(obj, name_key));
    }
}

static void collect_genres(JsonObject* obj, std::vector<std::string>& out) {
    if (!obj) return;
    collect_name_from_object_array(get_array_member(obj, "genres"), "name", out);
    collect_string_array(get_array_member(obj, "genre-list"), out);
    collect_name_from_object_array(get_array_member(obj, "tags"), "name", out);
    collect_string_array(get_array_member(obj, "tag-list"), out);
}

static void fill_track_tags_from_track(
    JsonObject* track_obj,
    const std::string& fallback_artist,
    std::vector<CdRipTagKV>& out_tags) {

    if (!track_obj) return;
    const std::string title = get_string_member(track_obj, "title");
    append_tag(out_tags, "TITLE", title);

    JsonArray* ac = get_array_member(track_obj, "artist-credit");
    const std::string ac_text = join_artist_credit(ac);
    const std::string track_artist = !ac_text.empty() ? ac_text : fallback_artist;
    append_tag(out_tags, "ARTIST", track_artist);

    const std::string track_id = get_string_member(track_obj, "id");
    append_tag(out_tags, "MUSICBRAINZ_TRACKID", track_id);

    JsonObject* recording = get_object_member(track_obj, "recording");
    if (recording) {
        append_tag(out_tags, "MUSICBRAINZ_RECORDINGID", get_string_member(recording, "id"));
        JsonArray* isrcs = get_array_member(recording, "isrcs");
        if (isrcs) {
            std::vector<std::string> values;
            const guint len = json_array_get_length(isrcs);
            for (guint i = 0; i < len; ++i) {
                const gchar* isrc = json_array_get_string_element(isrcs, i);
                if (isrc) values.emplace_back(isrc);
            }
            append_tag(out_tags, "ISRC", join_strings(values, "; "));
        }
        JsonArray* rec_ac = get_array_member(recording, "artist-credit");
        const std::string rec_artist = join_artist_credit(rec_ac);
        if (!rec_artist.empty()) {
            append_tag(out_tags, "ARTIST", rec_artist);
        }
    }
}

static bool build_entries_from_release(
    const CdRipDiscToc* toc,
    const std::string& request_url,
    JsonObject* release_obj,
    const std::vector<long>& offsets,
    const std::string& discid,
    std::vector<CdRipCddbEntry>& results) {

    if (!release_obj || !toc) return false;
    JsonArray* media_array = get_array_member(release_obj, "media");
    if (!media_array) return false;

    const std::string preferred_medium = to_string_or_empty(toc->mb_medium_id);
    std::vector<JsonObject*> media = select_matching_media(
        media_array, toc, offsets, discid, preferred_medium);
    if (media.empty()) return false;

    const std::string release_id = get_string_member(release_obj, "id");
    const std::string release_title = get_string_member(release_obj, "title");
    const std::string album_artist = join_artist_credit(get_array_member(release_obj, "artist-credit"));
    const std::string date = get_string_member(release_obj, "date");
    const std::string release_country = get_string_member(release_obj, "country");
    const std::string barcode = get_string_member(release_obj, "barcode");
    const std::string status = get_string_member(release_obj, "status");
    const int medium_total = static_cast<int>(json_array_get_length(media_array));
    JsonObject* release_group = get_object_member(release_obj, "release-group");
    const std::string release_group_id = get_string_member(release_group, "id");
    const std::string discogs_release_id = extract_discogs_release_id(release_obj);
    std::vector<std::string> genres;
    collect_genres(release_obj, genres);
    collect_genres(release_group, genres);
    const std::string genre_text = join_strings(genres, "; ");
    JsonObject* cover_art_archive = get_object_member(release_obj, "cover-art-archive");
    const bool has_cover_artwork =
        get_bool_member(cover_art_archive, "artwork", false) ||
        get_bool_member(cover_art_archive, "front", false);

    for (JsonObject* medium_obj : media) {
        if (!medium_obj) continue;
        std::vector<CdRipTagKV> album_tags;
        std::vector<std::vector<CdRipTagKV>> track_tags(toc->tracks_count);

        const std::string medium_id = get_string_member(medium_obj, "id");
        const std::string medium_title = get_string_member(medium_obj, "title");
        const std::string medium_format = get_string_member(medium_obj, "format");
        const int track_total = get_int_member(medium_obj, "track-count", -1);
        const int disc_number = get_int_member(medium_obj, "position", -1);

        append_tag(album_tags, "ALBUM", release_title);
        append_tag(album_tags, "ARTIST", album_artist);
        append_tag(album_tags, "ALBUMARTIST", album_artist);
        append_tag(album_tags, "DATE", date);
        append_tag(album_tags, "RELEASECOUNTRY", release_country);
        append_tag(album_tags, "BARCODE", barcode);
        append_tag(album_tags, "RELEASESTATUS", status);
        append_tag(album_tags, "GENRE", genre_text);
        append_tag(album_tags, "MEDIA", medium_format);
        append_tag(album_tags, "MUSICBRAINZ_RELEASE", release_id);
        append_tag(album_tags, "MUSICBRAINZ_MEDIUM", medium_id);
        append_tag(album_tags, "MUSICBRAINZ_MEDIUMTITLE", medium_title);
        append_tag(album_tags, "MUSICBRAINZ_RELEASEGROUPID", release_group_id);
        append_tag(album_tags, "DISCOGS_RELEASE", discogs_release_id);
        if (track_total > 0) append_tag(album_tags, "TRACKTOTAL", std::to_string(track_total));
        if (disc_number > 0) append_tag(album_tags, "DISCNUMBER", std::to_string(disc_number));
        if (medium_total > 0) append_tag(album_tags, "DISCTOTAL", std::to_string(medium_total));

        JsonArray* label_info = get_array_member(release_obj, "label-info");
        if (label_info) {
            const guint label_len = json_array_get_length(label_info);
            for (guint li = 0; li < label_len; ++li) {
                JsonObject* li_obj = json_array_get_object_element(label_info, li);
                if (!li_obj) continue;
                JsonObject* label = get_object_member(li_obj, "label");
                append_tag(album_tags, "LABEL", get_string_member(label, "name"));
                append_tag(album_tags, "CATALOGNUMBER", get_string_member(li_obj, "catalog-number"));
            }
        }

        JsonArray* tracks = get_array_member(medium_obj, "tracks");
        if (tracks) {
            const guint track_len = json_array_get_length(tracks);
            size_t fallback_index = 0;
            for (guint ti = 0; ti < track_len; ++ti) {
                JsonObject* track_obj = json_array_get_object_element(tracks, ti);
                if (!track_obj) continue;
                int position = get_int_member(track_obj, "position", -1);
                if (position <= 0) {
                    const std::string number = get_string_member(track_obj, "number");
                    if (!number.empty()) {
                        try { position = std::stoi(number); } catch (...) { position = -1; }
                    }
                }
                size_t index = (position > 0)
                    ? static_cast<size_t>(position - 1)
                    : fallback_index;
                if (index >= track_tags.size()) continue;
                fill_track_tags_from_track(track_obj, album_artist, track_tags[index]);
                ++fallback_index;
            }
        }

        CdRipCddbEntry entry{};
        entry.cddb_discid = make_cstr_copy(to_string_or_empty(toc->cddb_discid));
        entry.source_label = make_cstr_copy(kMusicBrainzLabel);
        entry.source_url = make_cstr_copy(request_url);
        char* ts = cdrip_current_timestamp_iso();
        entry.fetched_at = make_cstr_copy(ts);
        cdrip_release_timestamp(ts);
        if (has_cover_artwork) {
            entry.cover_art.available = 1;
            entry.cover_art.is_front = 1;
        }

        if (!album_tags.empty()) {
            entry.album_tags_count = album_tags.size();
            entry.album_tags = new CdRipTagKV[entry.album_tags_count]{};
            for (size_t i = 0; i < album_tags.size(); ++i) {
                entry.album_tags[i] = album_tags[i];
            }
        }
        entry.tracks_count = track_tags.size();
        if (!track_tags.empty()) {
            entry.tracks = new CdRipTrackTags[track_tags.size()]{};
            for (size_t i = 0; i < track_tags.size(); ++i) {
                if (!track_tags[i].empty()) {
                    entry.tracks[i].tags_count = track_tags[i].size();
                    entry.tracks[i].tags = new CdRipTagKV[track_tags[i].size()]{};
                    for (size_t k = 0; k < track_tags[i].size(); ++k) {
                        entry.tracks[i].tags[k] = track_tags[i][k];
                    }
                }
            }
        }

        results.push_back(std::move(entry));
    }

    return true;
}

static bool fetch_musicbrainz_entries(
    const CdRipDiscToc* toc,
    std::vector<CdRipCddbEntry>& results,
    std::string& err) {

    if (!toc || !toc->tracks || toc->tracks_count == 0) {
        err = "MusicBrainz query failed: invalid TOC";
        return false;
    }

    long mb_leadout = 0;
    std::vector<long> offsets = build_mb_offsets(toc, mb_leadout);
    if (offsets.empty()) {
        err = "MusicBrainz query failed: unable to build TOC";
        return false;
    }

    std::string discid = to_string_or_empty(toc->mb_discid);
    if (discid.empty()) {
        std::string computed;
        long temp_leadout = 0;
        if (compute_musicbrainz_discid(toc, computed, temp_leadout)) {
            discid = computed;
        }
    }
    const std::string release_id = to_string_or_empty(toc->mb_release_id);

    std::string url;
    bool use_release_endpoint = false;
    if (!release_id.empty()) {
        url = "https://musicbrainz.org/ws/2/release/" + release_id +
            "?fmt=json&inc=" + kMusicBrainzReleaseInc;
        use_release_endpoint = true;
    } else {
        const std::string toc_param = build_toc_param(toc);
        if (toc_param.empty()) {
            err = "MusicBrainz query failed: unable to build TOC";
            return false;
        }

        // Prefer release matches over CD stubs, and allow fuzzy TOC lookups even when a CD stub exists.
        // See: https://musicbrainz.org/doc/MusicBrainz_API#discid
        const std::string discid_path = discid.empty() ? "-" : discid;
        url = "https://musicbrainz.org/ws/2/discid/" + discid_path +
            "?fmt=json&toc=" + toc_param +
            "&cdstubs=no" +
            "&inc=" + kMusicBrainzInc;
    }

    std::string body;
    if (!http_get_json(url, body, err)) {
        return false;
    }

    JsonParser* parser = json_parser_new();
    GError* gerr = nullptr;
    if (!json_parser_load_from_data(parser, body.c_str(), body.size(), &gerr)) {
        err = gerr && gerr->message ? std::string{gerr->message} : "MusicBrainz response parse error";
        if (gerr) g_error_free(gerr);
        g_object_unref(parser);
        return false;
    }
    JsonNode* root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        err = "MusicBrainz response is not a JSON object";
        g_object_unref(parser);
        return false;
    }
    JsonObject* root_obj = json_node_get_object(root);

    if (use_release_endpoint) {
        build_entries_from_release(toc, url, root_obj, offsets, discid, results);
    } else {
        JsonArray* releases = get_array_member(root_obj, "releases");
        if (releases) {
            const guint len = json_array_get_length(releases);
            bool any_success = false;
            std::string last_err;
            for (guint i = 0; i < len; ++i) {
                JsonObject* release_obj = json_array_get_object_element(releases, i);
                if (!release_obj) continue;
                const std::string rid = get_string_member(release_obj, "id");
                if (rid.empty()) continue;
                std::string rel_err;
                if (fetch_release_details_and_build(toc, rid, offsets, discid, results, rel_err)) {
                    any_success = true;
                } else if (!rel_err.empty()) {
                    last_err = rel_err;
                }
            }
            if (!any_success) {
                // Fallback: build from the discid response if release lookups failed.
                for (guint i = 0; i < len; ++i) {
                    JsonObject* release_obj = json_array_get_object_element(releases, i);
                    if (!release_obj) continue;
                    build_entries_from_release(toc, url, release_obj, offsets, discid, results);
                }
                if (results.empty() && !last_err.empty()) {
                    err = last_err;
                    g_object_unref(parser);
                    return false;
                }
            }
        }
    }

    g_object_unref(parser);
    return true;
}

static bool fetch_musicbrainz_entries_by_title(
    const CdRipDiscToc* toc,
    const std::string& album_title,
    std::vector<CdRipCddbEntry>& results,
    std::string& err) {

    if (!toc || !toc->tracks || toc->tracks_count == 0) {
        err = "MusicBrainz query failed: invalid TOC";
        return false;
    }

    const std::string url = build_musicbrainz_release_search_url(album_title);
    if (url.empty()) return true;

    long mb_leadout = 0;
    std::vector<long> offsets = build_mb_offsets(toc, mb_leadout);
    if (offsets.empty()) {
        err = "MusicBrainz query failed: unable to build TOC";
        return false;
    }

    std::string discid = to_string_or_empty(toc->mb_discid);
    if (discid.empty()) {
        std::string computed;
        long temp_leadout = 0;
        if (compute_musicbrainz_discid(toc, computed, temp_leadout)) {
            discid = computed;
        }
    }

    std::string body;
    if (!http_get_json(url, body, err)) {
        return false;
    }

    JsonParser* parser = json_parser_new();
    GError* gerr = nullptr;
    if (!json_parser_load_from_data(parser, body.c_str(), body.size(), &gerr)) {
        err = gerr && gerr->message ? std::string{gerr->message} : "MusicBrainz response parse error";
        if (gerr) g_error_free(gerr);
        g_object_unref(parser);
        return false;
    }
    JsonNode* root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        err = "MusicBrainz response is not a JSON object";
        g_object_unref(parser);
        return false;
    }
    JsonObject* root_obj = json_node_get_object(root);
    JsonArray* releases = get_array_member(root_obj, "releases");
    if (releases) {
        const guint len = json_array_get_length(releases);
        bool any_success = false;
        std::string last_err;
        for (guint i = 0; i < len; ++i) {
            JsonObject* release_obj = json_array_get_object_element(releases, i);
            if (!release_obj) continue;
            const std::string rid = get_string_member(release_obj, "id");
            if (rid.empty()) continue;
            std::string rel_err;
            if (fetch_release_details_and_build(toc, rid, offsets, discid, results, rel_err)) {
                any_success = true;
            } else if (!rel_err.empty()) {
                last_err = rel_err;
            }
        }
        if (!any_success && !last_err.empty()) {
            err = last_err;
            g_object_unref(parser);
            return false;
        }
    }

    g_object_unref(parser);
    return true;
}

/* ------------------------------------------------------------------- */
/* Exported API functions */

namespace {

struct ServerFetchResult {
    std::vector<CdRipCddbEntry> entries;
    std::string error;
};

static ServerFetchResult fetch_entries_from_musicbrainz(
    const CdRipDiscToc* toc) {

    ServerFetchResult out;
    fetch_musicbrainz_entries(toc, out.entries, out.error);
    return out;
}

static ServerFetchResult fetch_entries_from_cddb_server(
    const CdRipDiscToc* toc,
    const CdRipCddbServer& server,
    const std::string& toc_discid) {

    ServerFetchResult out;
    const std::string server_label = to_string_or_empty(server.label);
    const std::string server_name = to_string_or_empty(server.name);
    const std::string server_path = to_string_or_empty(server.path);

    cddb_conn_t* conn = cddb_new();
    if (!conn) {
        out.error = "Failed to create CDDB connection for " + server_label;
        return out;
    }
    cddb_set_server_name(conn, server_name.c_str());
    cddb_set_server_port(conn, server.port);
    cddb_set_http_path_query(conn, server_path.c_str());
    cddb_http_enable(conn);

    cddb_disc_t* disc = cddb_disc_new();
    if (!disc) {
        out.error = "Failed to create CDDB disc object";
        cddb_destroy(conn);
        return out;
    }
    for (size_t ti = 0; ti < toc->tracks_count; ++ti) {
        const auto& t = toc->tracks[ti];
        cddb_track_t* track = cddb_track_new();
        cddb_track_set_frame_offset(track, static_cast<int>(t.start));
        cddb_disc_add_track(disc, track);
    }
    cddb_disc_set_length(disc, toc->length_seconds);
    cddb_disc_set_discid(
        disc,
        static_cast<unsigned int>(
            std::strtoul(toc_discid.c_str(), nullptr, 16)));

    const int matches = cddb_query(conn, disc);
    if (matches <= 0) {
        cddb_disc_destroy(disc);
        cddb_destroy(conn);
        return out;
    }

    std::ostringstream url_builder;
    url_builder << "http://" << server_name;
    if (server.port != 80 && server.port != 443) {
        url_builder << ":" << server.port;
    }
    url_builder << server_path;
    const std::string url = url_builder.str();

    int index = 0;
    do {
        cddb_disc_t* entry_disc = cddb_disc_clone(disc);
        if (!entry_disc) continue;
        cddb_read(conn, entry_disc);

        CdRipCddbEntry entry{};
        entry.cddb_discid = make_cstr_copy(toc_discid);
        entry.source_label = make_cstr_copy(server_label);
        entry.source_url = make_cstr_copy(url);
        char* ts = cdrip_current_timestamp_iso();
        entry.fetched_at = make_cstr_copy(ts);
        cdrip_release_timestamp(ts);
        std::vector<CdRipTagKV> album_tags;
        std::vector<std::vector<CdRipTagKV>> track_tags;
        auto add_album = [&](const std::string& key, const std::string& val) {
            album_tags.push_back(make_kv(key, val));
        };

        add_album("ARTIST", cddb_disc_get_artist(entry_disc) ? cddb_disc_get_artist(entry_disc) : "");
        add_album("ALBUM", cddb_disc_get_title(entry_disc) ? cddb_disc_get_title(entry_disc) : "");
        add_album("GENRE", cddb_disc_get_genre(entry_disc) ? cddb_disc_get_genre(entry_disc) : "");
        const int year = cddb_disc_get_year(entry_disc);
        if (year > 0) add_album("DATE", std::to_string(year));

        const int meta_tracks = cddb_disc_get_track_count(entry_disc);
        track_tags.resize(static_cast<size_t>(meta_tracks));
        for (int i = 0; i < meta_tracks; ++i) {
            cddb_track_t* t = cddb_disc_get_track(entry_disc, i); // 0-based
            std::string title = t && cddb_track_get_title(t)
                ? cddb_track_get_title(t) : "";
            if (title.empty()) {
                std::ostringstream oss;
                oss << "Track " << (i + 1);
                title = oss.str();
            }
            track_tags[static_cast<size_t>(i)].push_back(make_kv("TITLE", title));
        }

        if (!album_tags.empty()) {
            entry.album_tags_count = album_tags.size();
            entry.album_tags = new CdRipTagKV[entry.album_tags_count]{};
            for (size_t i = 0; i < album_tags.size(); ++i)
                entry.album_tags[i] = album_tags[i];
        }
        entry.tracks_count = track_tags.size();
        if (entry.tracks_count > 0) {
            entry.tracks = new CdRipTrackTags[entry.tracks_count]{};
            for (size_t ti = 0; ti < track_tags.size(); ++ti) {
                const auto& vec = track_tags[ti];
                if (!vec.empty()) {
                    entry.tracks[ti].tags_count = vec.size();
                    entry.tracks[ti].tags = new CdRipTagKV[vec.size()]{};
                    for (size_t k = 0; k < vec.size(); ++k) {
                        entry.tracks[ti].tags[k] = vec[k];
                    }
                }
            }
        }

        out.entries.push_back(entry);
        cddb_disc_destroy(entry_disc);
        ++index;
    } while (index < matches && cddb_query_next(conn, disc) == 1);

    cddb_disc_destroy(disc);
    cddb_destroy(conn);
    return out;
}

}  // namespace

extern "C" {

CdRipCddbEntryList* cdrip_fetch_cddb_entries(
    const CdRipDiscToc* toc,
    const CdRipCddbServerList* servers,
    const char** error) {

    clear_error(error);
    CdRipCddbEntryList* list = new CdRipCddbEntryList{};
    std::vector<CdRipCddbEntry> results;
    if (!toc || !toc->tracks || toc->tracks_count == 0) {
        set_error(error, "Invalid TOC provided");
        return list;
    }
    if (!servers || servers->count == 0) {
        set_error(error, "No CDDB servers specified");
        return list;
    }

    std::string toc_discid = to_string_or_empty(toc->cddb_discid);

    std::vector<std::future<ServerFetchResult>> futures;
    futures.reserve(servers->count);
    bool has_musicbrainz_server = false;
    size_t musicbrainz_insert_index = 0;
    std::vector<bool> is_musicbrainz_server(servers->count, false);
    for (size_t si = 0; si < servers->count; ++si) {
        const CdRipCddbServer server = servers->servers[si];
        const std::string server_label = to_string_or_empty(server.label);
        if (to_lower(server_label) == kMusicBrainzLabel) {
            if (!has_musicbrainz_server) {
                musicbrainz_insert_index = si;
            }
            has_musicbrainz_server = true;
            is_musicbrainz_server[si] = true;
        }
        futures.push_back(std::async(std::launch::async, [toc, server, toc_discid]() -> ServerFetchResult {
            const std::string label = to_string_or_empty(server.label);
            if (to_lower(label) == kMusicBrainzLabel) {
                return fetch_entries_from_musicbrainz(toc);
            }
            return fetch_entries_from_cddb_server(toc, server, toc_discid);
        }));
    }

    std::vector<ServerFetchResult> per_server;
    per_server.reserve(futures.size());
    for (auto& fut : futures) {
        try {
            per_server.push_back(fut.get());
        } catch (const std::exception& ex) {
            ServerFetchResult r;
            r.error = std::string{"CDDB fetch failed: "} + ex.what();
            per_server.push_back(std::move(r));
        } catch (...) {
            ServerFetchResult r;
            r.error = "CDDB fetch failed: unknown error";
            per_server.push_back(std::move(r));
        }
    }

    std::string mb_title_err;
    if (has_musicbrainz_server) {
        size_t mb_entries_count = 0;
        std::vector<const CdRipCddbEntry*> other_entries;
        size_t other_entries_count = 0;
        for (size_t si = 0; si < per_server.size(); ++si) {
            if (is_musicbrainz_server[si]) {
                mb_entries_count += per_server[si].entries.size();
            } else {
                other_entries_count += per_server[si].entries.size();
            }
        }
        other_entries.reserve(other_entries_count);
        for (size_t si = 0; si < per_server.size(); ++si) {
            if (is_musicbrainz_server[si]) {
                continue;
            }
            for (const auto& e : per_server[si].entries) {
                other_entries.push_back(&e);
            }
        }
        if (mb_entries_count == 0 && !other_entries.empty()) {
            const std::vector<std::string> candidates =
                extract_album_title_candidates(other_entries);
            if (!candidates.empty()) {
                std::unordered_set<std::string> seen_mb_keys;
                auto& target = per_server[musicbrainz_insert_index].entries;
                seen_mb_keys.reserve(target.size());
                for (const auto& e : target) {
                    const std::string key = build_musicbrainz_release_key(e);
                    if (!key.empty()) seen_mb_keys.insert(key);
                }
                for (const auto& candidate : candidates) {
                    std::vector<CdRipCddbEntry> mb_entries;
                    std::string candidate_err;
                    if (!fetch_musicbrainz_entries_by_title(toc, candidate, mb_entries, candidate_err)) {
                        if (!candidate_err.empty()) {
                            mb_title_err = candidate_err;
                        }
                        continue;
                    }
                    if (!mb_entries.empty()) {
                        for (auto& entry : mb_entries) {
                            const std::string key = build_musicbrainz_release_key(entry);
                            if (!key.empty() && !seen_mb_keys.insert(key).second) {
                                release_cddb_entry(entry);
                                continue;
                            }
                            target.push_back(entry);
                        }
                    }
                }
            }
        }
    }

    // Preserve the original server order when merging results and choosing the first error.
    for (const auto& r : per_server) {
        if (!r.error.empty()) set_error(error, r.error);
        results.insert(results.end(), r.entries.begin(), r.entries.end());
    }
    if (!mb_title_err.empty()) {
        set_error(error, std::string{"MusicBrainz title search failed: "} + mb_title_err);
    }

    if (!results.empty()) {
        list->count = results.size();
        list->entries = new CdRipCddbEntry[list->count]{};
        for (size_t i = 0; i < list->count; ++i) {
            list->entries[i] = results[i];
        }
    }
    return list;
}

void cdrip_release_cddbentry_list(
    CdRipCddbEntryList* p) {

    if (!p) return;
    if (p->entries) {
        for (size_t i = 0; i < p->count; ++i) {
            CdRipCddbEntry* e = &p->entries[i];
            release_cstr(e->cddb_discid);
            release_cstr(e->source_label);
            release_cstr(e->source_url);
            release_cstr(e->fetched_at);
            if (e->cover_art.data) {
                delete[] e->cover_art.data;
                e->cover_art.data = nullptr;
            }
            release_cstr(e->cover_art.mime_type);
            e->cover_art.size = 0;
            e->cover_art.available = 0;
            e->cover_art.is_front = 0;
            if (e->album_tags) {
                for (size_t ti = 0; ti < e->album_tags_count; ++ti) {
                    release_cstr(e->album_tags[ti].key);
                    release_cstr(e->album_tags[ti].value);
                }
                delete[] e->album_tags;
                e->album_tags = nullptr;
            }
            e->album_tags_count = 0;
            if (e->tracks) {
                for (size_t t = 0; t < e->tracks_count; ++t) {
                    CdRipTrackTags* tt = &e->tracks[t];
                    if (tt->tags) {
                        for (size_t kv = 0; kv < tt->tags_count; ++kv) {
                            release_cstr(tt->tags[kv].key);
                            release_cstr(tt->tags[kv].value);
                        }
                        delete[] tt->tags;
                        tt->tags = nullptr;
                    }
                    tt->tags_count = 0;
                }
                delete[] e->tracks;
                e->tracks = nullptr;
            }
            e->tracks_count = 0;
        }
        delete[] p->entries;
        p->entries = nullptr;
    }
    p->count = 0;
    delete p;
}

};
