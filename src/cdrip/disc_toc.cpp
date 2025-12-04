// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <stdint.h>

#include <sstream>

#include <cdio/cdio.h>
#include <cdio/cd_types.h>
#include <cddb/cddb.h>
#include <glib.h>

#include "internal.h"

using namespace cdrip::detail;

/* ------------------------------------------------------------------- */
/* Use static linkage for file local definitions */

static std::string to_hex(
    uint32_t value) {

    std::ostringstream oss;
    oss << std::hex << std::nouppercase << value;
    return oss.str();
}

/* ------------------------------------------------------------------- */

namespace cdrip::detail {

bool compute_musicbrainz_discid(
    const CdRipDiscToc* toc,
    std::string& out_discid,
    long& out_leadout) {

    if (!toc || !toc->tracks || toc->tracks_count == 0) return false;
    int first_track = toc->tracks[0].number;
    int last_track = toc->tracks[toc->tracks_count - 1].number;
    if (first_track <= 0 || last_track < first_track) return false;
    if (toc->tracks_count > 99) return false;

    // offsets[0]=leadout, [1..] track offsets, padded to 100 entries.
    int offsets[100] = {0};
    long leadout_raw = toc->leadout_sector > 0
        ? toc->leadout_sector
        : (toc->tracks[toc->tracks_count - 1].end + 1);
    long leadout = leadout_raw + 150;  // MusicBrainz expects +150 frames lead-in
    offsets[0] = static_cast<int>(leadout);
    for (size_t i = 0; i < toc->tracks_count; ++i) {
        offsets[i + 1] = static_cast<int>(toc->tracks[i].start + 150);  // MB offset = LBA + 150
    }

    // Per https://musicbrainz.org/doc/Disc_ID_Calculation :
    // SHA1 over hex string: first(%02X) + last(%02X) + 100 offsets(%08X)
    GString* hex = g_string_new(nullptr);
    g_string_append_printf(hex, "%02X", first_track);
    g_string_append_printf(hex, "%02X", last_track);
    for (int i = 0; i < 100; ++i) {
        g_string_append_printf(hex, "%08X", offsets[i]);
    }

    GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(checksum, reinterpret_cast<const guchar*>(hex->str), hex->len);
    guint8 digest[20];
    gsize digest_len = sizeof(digest);
    g_checksum_get_digest(checksum, digest, &digest_len);
    g_checksum_free(checksum);
    g_string_free(hex, true);
    if (digest_len == 0) return false;

    gchar* b64 = g_base64_encode(digest, digest_len);
    if (!b64) return false;
    // MusicBrainz base64 variant: '+'->'.', '/'->'_', '='->'-'
    for (char* p = b64; *p; ++p) {
        if (*p == '+') *p = '.';
        else if (*p == '/') *p = '_';
        else if (*p == '=') *p = '-';
    }
    out_discid = b64;
    g_free(b64);
    out_leadout = leadout;

    return !out_discid.empty();
}

}  // namespace cdrip::detail

/* ------------------------------------------------------------------- */
/* Exported API functions */

extern "C" {

CdRipDiscToc* cdrip_build_disc_toc(
    const CdRip* drive,
    const char** error) {

    clear_error(error);
    if (!drive) {
        set_error(error, "Drive handle is null");
        return nullptr;
    }
    CdRipDiscToc* toc = new CdRipDiscToc{};
    const int track_count = cdda_tracks(drive->drive);
    if (track_count <= 0) {
        set_error(error, "No tracks found on disc");
        delete toc;
        return nullptr;
    }
    toc->tracks_count = static_cast<size_t>(track_count);
    toc->tracks = new CdRipTrackInfo[toc->tracks_count]{};
    for (int i = 1; i <= track_count; ++i) {
        CdRipTrackInfo info;
        info.number = i;
        info.start = cdda_track_firstsector(drive->drive, i);
        info.end = cdda_track_lastsector(drive->drive, i);
        info.is_audio = cdda_track_audiop(drive->drive, i);
        toc->tracks[static_cast<size_t>(i - 1)] = info;
    }
    const long last_sector = cdda_disc_lastsector(drive->drive);
    if (last_sector < 0) {
        set_error(error, "Failed to read disc last sector");
        cdrip_release_disctoc(toc);
        return nullptr;
    }
    toc->leadout_sector = last_sector + 1;
    toc->length_seconds = static_cast<int>(
        (last_sector + 1) / CDIO_CD_FRAMES_PER_SEC);

    cddb_disc_t* disc = cddb_disc_new();
    if (disc) {
        for (int i = 1; i <= track_count; ++i) {
            const long offset = cdda_track_firstsector(drive->drive, i);
            cddb_track_t* track = cddb_track_new();
            cddb_track_set_frame_offset(track, static_cast<int>(offset));
            cddb_disc_add_track(disc, track);
        }
        cddb_disc_set_length(disc, toc->length_seconds);
        cddb_disc_calc_discid(disc);
        toc->cddb_discid = make_cstr_copy(
            to_hex(static_cast<uint32_t>(cddb_disc_get_discid(disc))));
        cddb_disc_destroy(disc);
    } else {
        set_error(error, "Failed to create CDDB disc object");
    }

    std::string mb_discid;
    long mb_leadout = 0;
    if (compute_musicbrainz_discid(toc, mb_discid, mb_leadout)) {
        toc->mb_discid = make_cstr_copy(mb_discid);
    }
    return toc;
}

void cdrip_release_disctoc(
    CdRipDiscToc* p) {

    if (!p) return;
    release_cstr(p->mb_discid);
    release_cstr(p->mb_release_id);
    release_cstr(p->mb_medium_id);
    release_cstr(p->cddb_discid);
    p->leadout_sector = 0;
    delete[] p->tracks;
    p->tracks = nullptr;
    p->tracks_count = 0;
    delete p;
}

};
