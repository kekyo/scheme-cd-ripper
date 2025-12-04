#pragma once

// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#ifndef __CDRIP_H
#define __CDRIP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- */

/**
 * Release error string allocated by library functions.
 * @param p Pointer returned via error out-parameters (nullable).
 */
void cdrip_release_error(const char* p);

/* ------------------------------------------------------------------- */

/**
 * Get current timestamp in ISO-8601 format with timezone offset.
 * Caller must free with cdrip_release_timestamp.
 * @return Newly allocated string with timestamp.
 */
char* cdrip_current_timestamp_iso();
/**
 * Release a timestamp string allocated by cdrip_current_timestamp_iso.
 * @param p Pointer to free (nullable).
 */
void cdrip_release_timestamp(char* p);

/* ------------------------------------------------------------------- */

/** CDDB server endpoint definition. */
typedef struct CdRipCddbServer {
    /** Host/FQDN of the server. */
    const char* name;
    /** Port number to connect. */
    int port;
    /** CGI path for CDDB query. */
    const char* path;
    /** Display name for the source. */
    const char* label;
} CdRipCddbServer;

/** List of CDDB servers to query in order. */
typedef struct CdRipCddbServerList {
    /** Array of server definitions. */
    CdRipCddbServer* servers;
    /** Number of entries in servers. */
    size_t count;
} CdRipCddbServerList;

/**
 * Rip mode configuration.
 */
typedef enum CdRipRipModes {
    /** Fast (disable integrity checks). */
    RIP_MODES_FAST = 0,
    /** Best (enable integrity checks). */
    RIP_MODES_BEST = 1,
    /** Default (currently maps to best). */
    RIP_MODES_DEFAULT = 2,
} CdRipRipModes;

/** Global configuration loaded from INI or defaults. */
typedef struct CdRipConfig {
    /** CD device path (nullable => auto-detect). */
    const char* device;
    /** Output filename/dirname format template. */
    const char* format;
    /** FLAC compression (0-8, <0 => auto). */
    int compression_level;
    /** Rip mode. */
    CdRipRipModes mode;
    /** Repeat prompt for next disc. */
    bool repeat;
    /** Sort CDDB results by album. */
    bool sort;
    /** Auto mode (non-interactive drive/CDDB selection). */
    bool auto_mode;
    /** CDDB server list (owned). */
    CdRipCddbServerList* servers;
    /** Loaded config file path, or null when defaults. */
    const char* config_path;
} CdRipConfig;

/**
 * Load configuration from INI file.
 * Search order when path is null: ./cdrip.conf then ~/.cdrip.conf.
 * Returns defaults if no file found; returns null on parse/load error.
 * @param path Optional explicit config path.
 * @param error Optional error string out-parameter.
 * @return Newly allocated config, must free with cdrip_release_config; null on failure.
 */
CdRipConfig* cdrip_load_config(
    const char* path /* nullable */,
    const char** error /* nullable */);
/**
 * Release configuration and owned members.
 * @param cfg Config pointer (nullable).
 */
void cdrip_release_config(
    CdRipConfig* cfg);

/* ------------------------------------------------------------------- */

/** Detected CD drive information. */
typedef struct CdRipDetectedDrive {
    /** Device path. */
    const char* device;
    /** Non-zero if media is present. */
    int has_media;
} CdRipDetectedDrive;

/** List of detected CD drives. */
typedef struct CdRipDetectedDriveList {
    /** Array of detected drives. */
    CdRipDetectedDrive* drives;
    /** Number of detected drives. */
    size_t count;
} CdRipDetectedDriveList;

/**
 * Detect available CD drives using libcdio.
 * @return Newly allocated list; free with cdrip_release_detecteddrive_list.
 */
CdRipDetectedDriveList* cdrip_detect_cd_drives();
/**
 * Release detected drive list.
 * @param p List pointer (nullable).
 */
void cdrip_release_detecteddrive_list(
    CdRipDetectedDriveList* p);

/* ------------------------------------------------------------------- */

/** Per-run settings for opening the drive. */
typedef struct CdRipSettings {
    /** Output filename/dirname format template. */
    const char* format;
    /** FLAC compression (0-8, <0 => auto). */
    int compression_level;
    /** Rip mode. */
    CdRipRipModes mode;
} CdRipSettings;

/** Opaque handle for Scheme CD ripper. */
typedef struct CdRip CdRip;

/**
 * Open Scheme CD Ripper.
 * @param device CD device path (nullable => auto-detect not handled here).
 * @param settings Rip settings (nullable for defaults).
 * @param error Optional error string out-parameter.
 * @return Handle to ripper on success; null on failure.
 */
CdRip* cdrip_open(
    const char* device,
    const CdRipSettings* settings,
    const char** error /* nullable */);
/**
 * Close Scheme CD Ripper and optionally eject the disc.
 * @param d Ripper handle.
 * @param will_eject True to eject the disc.
 * @param error Optional error string out-parameter.
 */
void cdrip_close(
    CdRip* d,
    bool will_eject,
    const char** error /* nullable */);

/* ------------------------------------------------------------------- */

/** Track information from TOC. */
typedef struct CdRipTrackInfo {
    /** Track number (1-based). */
    int number;
    /** Start sector. */
    long start;
    /** End sector. */
    long end;
    /** Non-zero if audio track. */
    int is_audio;
} CdRipTrackInfo;

/** Disc TOC information. */
typedef struct CdRipDiscToc {
    /** CDDB disc ID (hex). */
    const char* cddb_discid;
    /** MusicBrainz disc ID (base64 variant). */
    const char* mb_discid;
    /** MusicBrainz release ID (UUID). */
    const char* mb_release_id;
    /** MusicBrainz medium ID (UUID). */
    const char* mb_medium_id;
    /** Leadout sector (last sector + 1). */
    long leadout_sector;
    /** Disc length in seconds. */
    int length_seconds;
    /** Array of tracks. */
    CdRipTrackInfo* tracks;
    /** Number of tracks. */
    size_t tracks_count;
} CdRipDiscToc;

/** Generic tag key/value. */
typedef struct CdRipTagKV {
    const char* key;
    const char* value;
} CdRipTagKV;

/** Cover art image (front cover). */
typedef struct CdRipCoverArt {
    /** Raw image bytes (owned). */
    const uint8_t* data;
    /** Size of image bytes. */
    size_t size;
    /** MIME type (e.g. image/jpeg, owned). */
    const char* mime_type;
    /** Non-zero when this image is a front cover. */
    int is_front;
    /** Non-zero when MusicBrainz metadata indicates artwork exists. */
    int available;
} CdRipCoverArt;

/** Per-track tag list. */
typedef struct CdRipTrackTags {
    CdRipTagKV* tags;
    size_t tags_count;
} CdRipTrackTags;

/**
 * Build disc TOC information from an opened drive.
 * @param drive Ripper handle.
 * @param error Optional error string out-parameter.
 * @return Newly allocated TOC; free with cdrip_release_disctoc.
 */
CdRipDiscToc* cdrip_build_disc_toc(
    const CdRip* drive,
    const char** error /* nullable */);
/**
 * Release TOC.
 * @param p TOC pointer (nullable).
 */
void cdrip_release_disctoc(
    CdRipDiscToc* p);

/* ------------------------------------------------------------------- */

/** CDDB entry (album metadata). */
typedef struct CdRipCddbEntry {
    /** Disc ID (CDDB/MusicBrainz). */
    const char* cddb_discid;
    /** Label of source server. */
    const char* source_label;
    /** URL of source server. */
    const char* source_url;
    /** ISO timestamp when fetched. */
    const char* fetched_at;
    /** Album-level tags (key/value). */
    CdRipTagKV* album_tags;
    size_t album_tags_count;
    /** Track-level tag sets (length == tracks_count). */
    CdRipTrackTags* tracks;
    size_t tracks_count;
    /** Cover art info and cached image data (front cover only). */
    CdRipCoverArt cover_art;
} CdRipCddbEntry;

/** List of CDDB entries. */
typedef struct CdRipCddbEntryList {
    /** Array of entries. */
    CdRipCddbEntry* entries;
    /** Number of entries. */
    size_t count;
} CdRipCddbEntryList;

/**
 * Query multiple CDDB servers with the provided disc TOC.
 * @param toc Disc TOC.
 * @param servers Server list to query.
 * @param error Optional error string out-parameter.
 * @return Aggregated entry list; free with cdrip_release_cddbentry_list.
 */
CdRipCddbEntryList* cdrip_fetch_cddb_entries(
    const CdRipDiscToc* toc,
    const CdRipCddbServerList* servers,
    const char** error /* nullable */);
/**
 * Fetch front cover art from Cover Art Archive using MusicBrainz metadata.
 * On success, stores image bytes and MIME type into the entry's cover_art field.
 * @param entry Target CDDB entry (must come from MusicBrainz to be effective).
 * @param toc Disc TOC (used for fallback IDs, nullable).
 * @param error Optional error string out-parameter.
 * @return Non-zero on success (image obtained), zero on failure or not applicable.
 */
int cdrip_fetch_cover_art(
    CdRipCddbEntry* entry,
    const CdRipDiscToc* toc /* nullable */,
    const char** error /* nullable */);
/**
 * Release CDDB entry list.
 * @param p Entry list pointer (nullable).
 */
void cdrip_release_cddbentry_list(
    CdRipCddbEntryList* p);

/* ------------------------------------------------------------------- */

/** Tagged TOC read from an existing FLAC file. */
typedef struct CdRipTaggedToc {
    /** FLAC file path. */
    const char* path;
    /** Reconstructed disc TOC from Vorbis comments. */
    CdRipDiscToc* toc;
    /** Track number within the disc (nullable, 0 if unknown). */
    int track_number;
    /** Non-zero if TOC is valid for CDDB query. */
    int valid;
    /** Reason string when invalid (nullable). */
    const char* reason;
} CdRipTaggedToc;

/** List of tagged TOCs. */
typedef struct CdRipTaggedTocList {
    /** Array of items. */
    CdRipTaggedToc* items;
    /** Number of items. */
    size_t count;
} CdRipTaggedTocList;

/**
 * Collect CDDB query information from FLAC files under the path.
 * If path is a directory, recursively enumerates *.flac files.
 * Each entry indicates whether it is valid for CDDB query.
 * @param path FLAC file or directory path.
 * @param error Optional error string out-parameter.
 * @return List of tagged TOCs; free with cdrip_release_taggedtoc_list.
 */
CdRipTaggedTocList* cdrip_collect_cddb_queries_from_path(
    const char* path,
    const char** error /* nullable */);

/**
 * Release tagged TOC list.
 * @param p List pointer (nullable).
 */
void cdrip_release_taggedtoc_list(
    CdRipTaggedTocList* p);

/**
 * Update an existing FLAC file's tags using a selected CDDB entry.
 * @param tagged Source tagged TOC (path + toc + track number).
 * @param entry Selected CDDB entry.
 * @param error Optional error string out-parameter.
 * @return Non-zero on success.
 */
int cdrip_update_flac_with_cddb_entry(
    const CdRipTaggedToc* tagged,
    const CdRipCddbEntry* entry,
    const char** error /* nullable */);

/* ------------------------------------------------------------------- */

/** Progress information passed to callback during ripping. */
typedef struct CdRipProgressInfo {
    /** Current track number. */
    int track_number;
    /** Total tracks being ripped. */
    int total_tracks;
    /** Overall percent of album. */
    double percent;
    /** Elapsed seconds for current track (audio time). */
    double elapsed_track_sec;
    /** Total seconds for current track (audio time). */
    double track_total_sec;
    /** Elapsed seconds for album (audio time). */
    double elapsed_total_sec;
    /** Total seconds for album (audio time). */
    double total_album_sec;
    /** Wall-clock elapsed seconds since album start. */
    double wall_elapsed_sec;
    /** Estimated wall-clock total seconds for album. */
    double wall_total_sec;
    /** Wall-clock elapsed seconds for track. */
    double wall_track_elapsed_sec;
    /** Estimated wall-clock total seconds for track. */
    double wall_track_total_sec;
    /** Track title. */
    const char* title;
    /** Track title (newline free) */
    const char* track_name;
    /** Sanitized track title. */
    const char* safe_title;
    /** Destination path/URI currently writing. */
    const char* path;
} CdRipProgressInfo;

/** Progress callback signature. */
typedef void (*CdRipProgressCallback)(const CdRipProgressInfo*);

/**
 * Rip a single track to FLAC.
 * @param drive Ripper handle.
 * @param track Track info to rip.
 * @param meta CDDB metadata for the album.
 * @param toc Disc TOC information (for CDDB tags).
 * @param progress Progress callback (nullable).
 * @param error Optional error string out-parameter.
 * @param total_tracks Total number of audio tracks being ripped.
 * @param completed_before_sec Seconds of album audio completed before this track.
 * @param total_album_sec Total album audio seconds.
 * @param wall_start_sec Wall-clock start timestamp (seconds since epoch) for album.
 * @return Non-zero on success, zero on failure.
 */
int cdrip_rip_track(
    CdRip* drive,
    const CdRipTrackInfo* track,
    const CdRipCddbEntry* meta,
    const CdRipDiscToc* toc,
    CdRipProgressCallback progress,
    const char** error /* nullable */,
    int total_tracks,
    double completed_before_sec,
    double total_album_sec,
    double wall_start_sec);

#ifdef __cplusplus
}
#endif

#endif
