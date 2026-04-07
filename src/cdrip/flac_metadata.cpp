// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <cstdlib>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <cdio/cdio.h>
#include <cdio/cd_types.h>

#include "internal.h"

using namespace cdrip::detail;

/* ------------------------------------------------------------------- */
/* Use static linkage for file local definitions */

static bool is_flac_file(
    const std::filesystem::path& path) {

    const std::string ext = to_lower(path.extension().string());
    return ext == ".flac";
}

static bool collect_vorbis_comments(
    const std::string& path,
    std::map<std::string, std::string>& out) {

    FLAC__StreamMetadata* tags = nullptr;
    if (!FLAC__metadata_get_tags(path.c_str(), &tags)) {
        return false;
    }
    if (!tags || tags->type != FLAC__METADATA_TYPE_VORBIS_COMMENT) {
        if (tags) FLAC__metadata_object_delete(tags);
        return false;
    }
    const auto& vc = tags->data.vorbis_comment;
    for (uint32_t i = 0; i < vc.num_comments; ++i) {
        const auto& entry = vc.comments[i];
        char* name = nullptr;
        char* value = nullptr;
        if (FLAC__metadata_object_vorbiscomment_entry_to_name_value_pair(
                entry, &name, &value)) {
            std::string key = to_upper(to_string_or_empty(name));
            std::string val = to_string_or_empty(value);
            out[key] = val;
        }
        if (name) free(name);
        if (value) free(value);
    }
    FLAC__metadata_object_delete(tags);
    return true;
}

static std::vector<long> parse_offsets(
    const std::string& value,
    bool& ok) {

    std::vector<long> offsets;
    std::string token;
    ok = true;
    std::istringstream iss(value);
    while (std::getline(iss, token, ',')) {
        token = trim(token);
        if (token.empty()) continue;
        size_t start = 0;
        while (start < token.size()) {
            const size_t space = token.find_first_of(" \t", start);
            std::string part = token.substr(
                start,
                space == std::string::npos ? std::string::npos : space - start);
            part = trim(part);
            if (!part.empty()) {
                long offset = 0;
                if (!parse_long(part, offset)) {
                    ok = false;
                    return {};
                }
                offsets.push_back(offset);
            }
            if (space == std::string::npos) break;
            start = space + 1;
        }
    }
    return offsets;
}

static void set_invalid_tagged_toc(
    CdRipTaggedToc& item,
    const std::string& path,
    const std::string& reason,
    int track_number) {

    item.path = make_cstr_copy(path);
    item.toc = nullptr;
    item.track_number = track_number;
    item.valid = 0;
    item.reason = make_cstr_copy(reason);
}

/* ------------------------------------------------------------------- */
/* Exported API functions */

namespace cdrip::detail {

bool update_flac_tags(
    const std::string& path,
    const CdRipDiscToc* toc,
    int track_number,
    const CdRipCddbEntry* entry,
    const std::map<std::string, std::string>& extra_tags,
    bool preserve_replaygain_tags,
    std::string& err) {

    err.clear();
    if (!toc || !entry || path.empty()) {
        err = "Invalid arguments to update_flac_tags";
        return false;
    }

    const int safe_track_number = track_number > 0 ? track_number : 1;
    const int track_total = static_cast<int>(toc->tracks_count);
    CdRipTrackInfo synthetic_track{};
    synthetic_track.number = safe_track_number;
    synthetic_track.is_audio = 1;

    const CdRipTrackInfo* track_info = &synthetic_track;
    if (track_number > 0 &&
        toc->tracks &&
        static_cast<size_t>(track_number - 1) < toc->tracks_count) {
        track_info = &toc->tracks[static_cast<size_t>(track_number - 1)];
    }

    std::string title;
    std::string track_name;
    std::string safe_title;
    std::map<std::string, std::string> tags = build_track_vorbis_tags(
        track_info,
        entry,
        toc,
        track_total,
        title,
        track_name,
        safe_title);

    if (preserve_replaygain_tags) {
        std::map<std::string, std::string> existing_tags;
        if (collect_vorbis_comments(path, existing_tags)) {
            for (const auto& [key, value] : existing_tags) {
                const std::string key_upper = to_upper(key);
                if (is_replaygain_tag_key(key_upper) && !value.empty()) {
                    tags[key_upper] = value;
                }
            }
        }
    }
    for (const auto& [key, value] : extra_tags) {
        if (!key.empty() && !value.empty()) {
            tags[to_upper(key)] = value;
        }
    }

    prune_empty_tags(tags);
    drop_format_only_tags(tags);

    const bool replace_picture = has_cover_art_data(entry->cover_art);
    FLAC__Metadata_Chain* chain = FLAC__metadata_chain_new();
    if (!chain) {
        err = "Failed to create FLAC metadata chain";
        return false;
    }
    if (!FLAC__metadata_chain_read(chain, path.c_str())) {
        err = "Failed to read FLAC metadata: " + path;
        FLAC__metadata_chain_delete(chain);
        return false;
    }

    FLAC__Metadata_Iterator* it = FLAC__metadata_iterator_new();
    if (!it) {
        err = "Failed to create FLAC metadata iterator";
        FLAC__metadata_chain_delete(chain);
        return false;
    }

    FLAC__metadata_iterator_init(it, chain);
    while (true) {
        FLAC__StreamMetadata* block = FLAC__metadata_iterator_get_block(it);
        if (block && block->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            FLAC__metadata_iterator_delete_block(it, true);
            if (!FLAC__metadata_iterator_get_block(it) &&
                !FLAC__metadata_iterator_next(it)) {
                break;
            }
            continue;
        }
        if (replace_picture && block && block->type == FLAC__METADATA_TYPE_PICTURE) {
            FLAC__metadata_iterator_delete_block(it, true);
            if (!FLAC__metadata_iterator_get_block(it) &&
                !FLAC__metadata_iterator_next(it)) {
                break;
            }
            continue;
        }
        if (!FLAC__metadata_iterator_next(it)) break;
    }

    FLAC__metadata_iterator_init(it, chain);
    while (FLAC__metadata_iterator_next(it)) {
    }

    FLAC__StreamMetadata* vorbis = build_vorbis_comments(tags);
    if (!vorbis) {
        err = "Failed to build Vorbis comments";
        FLAC__metadata_iterator_delete(it);
        FLAC__metadata_chain_delete(chain);
        return false;
    }
    if (!FLAC__metadata_iterator_insert_block_after(it, vorbis)) {
        FLAC__metadata_object_delete(vorbis);
        err = "Failed to insert Vorbis comment block";
        FLAC__metadata_iterator_delete(it);
        FLAC__metadata_chain_delete(chain);
        return false;
    }

    if (replace_picture) {
        FLAC__StreamMetadata* picture = build_picture_block(entry->cover_art);
        if (!picture) {
            FLAC__metadata_object_delete(vorbis);
            err = "Failed to build picture block";
            FLAC__metadata_iterator_delete(it);
            FLAC__metadata_chain_delete(chain);
            return false;
        }
        if (!FLAC__metadata_iterator_insert_block_after(it, picture)) {
            FLAC__metadata_object_delete(vorbis);
            FLAC__metadata_object_delete(picture);
            err = "Failed to insert picture block";
            FLAC__metadata_iterator_delete(it);
            FLAC__metadata_chain_delete(chain);
            return false;
        }
    }

    if (!FLAC__metadata_chain_write(chain, true, true)) {
        err = "Failed to write FLAC metadata";
        FLAC__metadata_iterator_delete(it);
        FLAC__metadata_chain_delete(chain);
        return false;
    }

    FLAC__metadata_iterator_delete(it);
    FLAC__metadata_chain_delete(chain);
    return true;
}

}  // namespace cdrip::detail

extern "C" {

CdRipTaggedTocList* cdrip_collect_cddb_queries_from_path(
    const char* path,
    const char** error) {

    clear_error(error);
    CdRipTaggedTocList* list = new CdRipTaggedTocList{};
    if (!path) {
        set_error(error, "Path is null");
        return list;
    }

    const std::filesystem::path root(path);

    std::vector<std::filesystem::path> targets;
    std::error_code ec;

    // Is path directory?
    if (std::filesystem::is_directory(root, ec)) {
        for (auto it = std::filesystem::recursive_directory_iterator(root); it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file()) {
                if (is_flac_file(it->path())) {
                    targets.push_back(it->path());
                }
            }
        }
    }
    // Is path FLAC file?
    else if (std::filesystem::is_regular_file(root, ec)) {
        if (is_flac_file(root)) targets.push_back(root);
    } else {
        set_error(error, "Path not found or unsupported: " + std::string(path));
        return list;
    }

    std::vector<CdRipTaggedToc> items;
    for (const auto& p : targets) {
        CdRipTaggedToc item{};
        const std::string path_str = p.string();
        std::map<std::string, std::string> tags;
        if (!collect_vorbis_comments(path_str, tags)) {
            set_invalid_tagged_toc(item, path_str, "Failed to read Vorbis comments", 0);
            items.push_back(item);
            continue;
        }

        auto get_tag = [&](const std::string& key) -> std::string {
            const auto it = tags.find(to_upper(key));
            return it != tags.end() ? trim(it->second) : std::string{};
        };

        const std::string cddb_discid = get_tag("CDDB_DISCID");
        const std::string offsets_raw = get_tag("CDDB_OFFSETS");
        const std::string total_sec_raw = get_tag("CDDB_TOTAL_SECONDS");
        const std::string tracktotal_raw = get_tag("TRACKTOTAL");
        const std::string tracknumber_raw = get_tag("TRACKNUMBER");
        const std::string mb_release_id = get_tag("MUSICBRAINZ_RELEASE");
        const std::string mb_medium_id = get_tag("MUSICBRAINZ_MEDIUM");
        const std::string mb_discid_tag = get_tag("MUSICBRAINZ_DISCID");
        const std::string mb_leadout_tag = get_tag("MUSICBRAINZ_LEADOUT");
        const bool has_mb_leadout_tag = !mb_leadout_tag.empty();

        int track_total = 0;
        parse_int(tracktotal_raw, track_total);
        int track_number = 0;
        parse_int(tracknumber_raw, track_number);
        int total_seconds = 0;
        parse_int(total_sec_raw, total_seconds);
        bool offsets_ok = false;
        std::vector<long> offsets = parse_offsets(offsets_raw, offsets_ok);

        if (!offsets_ok) {
            set_invalid_tagged_toc(item, path_str, "Invalid CDDB_OFFSETS", track_number);
            items.push_back(item);
            continue;
        }
        if (track_total == 0) {
            track_total = static_cast<int>(offsets.size());
        }
        if (cddb_discid.empty() || offsets.empty() || total_seconds <= 0 || track_total <= 0) {
            set_invalid_tagged_toc(item, path_str, "Missing CDDB tags", track_number);
            items.push_back(item);
            continue;
        }
        if (static_cast<size_t>(track_total) != offsets.size()) {
            set_invalid_tagged_toc(item, path_str, "Offsets count mismatch with track total", track_number);
            items.push_back(item);
            continue;
        }

        long disc_frames = static_cast<long>(total_seconds) * CDIO_CD_FRAMES_PER_SEC;
        if (disc_frames <= 0) {
            set_invalid_tagged_toc(item, path_str, "Invalid disc length", track_number);
            items.push_back(item);
            continue;
        }

        bool offsets_sorted = true;
        for (size_t i = 1; i < offsets.size(); ++i) {
            if (offsets[i] <= offsets[i - 1]) {
                offsets_sorted = false;
                break;
            }
        }
        if (!offsets_sorted) {
            set_invalid_tagged_toc(item, path_str, "Offsets are not strictly increasing", track_number);
            items.push_back(item);
            continue;
        }

        CdRipDiscToc* toc = new CdRipDiscToc{};
        toc->cddb_discid = make_cstr_copy(cddb_discid);
        if (!mb_release_id.empty()) {
            toc->mb_release_id = make_cstr_copy(mb_release_id);
        }
        if (!mb_medium_id.empty()) {
            toc->mb_medium_id = make_cstr_copy(mb_medium_id);
        }
        if (!mb_discid_tag.empty()) {
            toc->mb_discid = make_cstr_copy(mb_discid_tag);
        }
        if (!mb_leadout_tag.empty()) {
            long mb_leadout = 0;
            if (parse_long(mb_leadout_tag, mb_leadout) && mb_leadout > 150) {
                toc->leadout_sector = mb_leadout - 150;
            }
        }

        if (toc->leadout_sector <= 0) {
            toc->leadout_sector = disc_frames;
        }
        toc->length_seconds = total_seconds;
        toc->tracks_count = offsets.size();
        toc->tracks = new CdRipTrackInfo[toc->tracks_count]{};
        bool length_ok = true;
        for (size_t i = 0; i < offsets.size(); ++i) {
            CdRipTrackInfo info{};
            info.number = static_cast<int>(i + 1);
            info.start = offsets[i];
            const long end = (i + 1 < offsets.size())
                ? offsets[i + 1] - 1
                : disc_frames - 1;
            if (end < info.start) {
                length_ok = false;
                break;
            }
            info.end = end;
            info.is_audio = 1;
            toc->tracks[i] = info;
        }
        if (!length_ok) {
            cdrip_release_disctoc(toc);
            set_invalid_tagged_toc(item, path_str, "Offsets length inconsistency", track_number);
            items.push_back(item);
            continue;
        }

        // Compute MusicBrainz disc id from reconstructed TOC for later queries.
        if (!toc->mb_discid && has_mb_leadout_tag) {
            std::string mb_discid;
            long mb_leadout = 0;
            if (compute_musicbrainz_discid(toc, mb_discid, mb_leadout)) {
                toc->mb_discid = make_cstr_copy(mb_discid);
            }
        }

        item.path = make_cstr_copy(path_str);
        item.toc = toc;
        item.track_number = track_number;
        item.valid = 1;
        item.reason = nullptr;
        items.push_back(item);
    }

    if (!items.empty()) {
        list->count = items.size();
        list->items = new CdRipTaggedToc[list->count]{};
        for (size_t i = 0; i < list->count; ++i) {
            list->items[i] = items[i];
        }
    }
    return list;
}

void cdrip_release_taggedtoc_list(
    CdRipTaggedTocList* p) {

    if (!p) return;
    if (p->items) {
        for (size_t i = 0; i < p->count; ++i) {
            CdRipTaggedToc* it = &p->items[i];
            release_cstr(it->path);
            release_cstr(it->reason);
            cdrip_release_disctoc(it->toc);
            it->toc = nullptr;
        }
        delete[] p->items;
        p->items = nullptr;
    }
    p->count = 0;
    delete p;
}

int cdrip_update_flac_with_cddb_entry(
    const CdRipTaggedToc* tagged,
    const CdRipCddbEntry* entry,
    const char** error) {

    clear_error(error);
    if (!tagged || !entry || !tagged->path || !tagged->toc) {
        set_error(error, "Invalid arguments to cdrip_update_flac_with_cddb_entry");
        return 0;
    }

    std::string err;
    if (!update_flac_tags(
            to_string_or_empty(tagged->path),
            tagged->toc,
            tagged->track_number,
            entry,
            {},
            /*preserve_replaygain_tags=*/true,
            err)) {
        set_error(error, err);
        return 0;
    }
    return 1;
}

};
