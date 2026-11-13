#pragma once
#include "accuraterip.h"
#include "device_settings.h"
#include <functional>
#include <iosfwd>
#include <optional>

namespace cdrip::detail {
/** A published candidate, or a reason why the catalogue supplied none. */
struct DriveOffsetLookup {
    std::optional<int> samples;
    std::string detail;
};

/** Interpret the public drive catalogue using exact normalized vendor/model matching. */
DriveOffsetLookup parse_drive_offsets(const std::string& html, const DriveIdentity& identity);
/** Fetch the public catalogue and obtain a candidate for this hardware. */
DriveOffsetLookup lookup_drive_offset(const DriveIdentity& identity);

/** A full-track checksum match for one offset and one database record. */
struct OffsetMatch {
    int samples;
    size_t record;
    unsigned confidence;
    bool v1;
    bool v2;
};

/** Measurements for one track; errors never substitute missing samples with evidence. */
struct OffsetTrackResult {
    unsigned number;
    std::optional<AccurateRipChecksum> listed_checksum;
    std::vector<OffsetMatch> matches;
    std::string error;
};

/** Independent results for the inserted CD, without a recommended drive setting. */
struct OffsetMeasurement {
    unsigned requested_tracks;
    std::optional<int> listed_offset;
    bool searched{false};
    int search_min{0};
    int search_max{0};
    std::vector<OffsetTrackResult> tracks;
    std::string error;
};

/** Progress arguments are track number, copied frames, and total frames. */
using MeasurementProgress = std::function<void(unsigned, size_t, size_t)>;

/** Capture PCM and compare selected full tracks with database records.
 * @param rip Open drive; existing configured correction is not applied.
 * @param toc Full original disc layout.
 * @param database Records for that layout.
 * @param listed_offset Optional initial candidate from the drive catalogue.
 * @param tracks Positive requested track count, clipped to available tracks.
 * @param progress Optional callback value; an empty function disables progress.
 * @return Per-track values and matches, or the reason measurement was unavailable.
 */
OffsetMeasurement measure_offsets(CdRip* rip, const CdRipDiscToc& toc,
    const AccurateRipDatabase& database, std::optional<int> listed_offset,
    unsigned tracks, const MeasurementProgress& progress);

/** Render values, track matches and common record matches for one CD. */
void print_offset_measurement(std::ostream& out, const OffsetMeasurement& result);

/** Injectable network lookups used by the measurement command. */
struct OffsetLookupServices {
    DriveOffsetLookup (*drive_offset)(const DriveIdentity&);
    AccurateRipDatabase (*disc_database)(const CdRipDiscToc&);
};

/** Measure the inserted disc and finish without encoding, ejecting, or updating configuration. */
int run_offset_measurement(CdRip* rip, unsigned tracks, const OffsetLookupServices& services, std::ostream& out);
}
