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
    /** Inclusive range with all required audio available; absent if none was searched. */
    std::optional<std::pair<int, int>> search_range{};
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

/** Progress arguments are phase label, track number, completed units, and total units. */
using MeasurementProgress = std::function<void(const char*, unsigned, size_t, size_t)>;

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

/** Render the track and disc matches, then a configuration example for a single matching offset.
 * @param out Measurement output stream.
 * @param result Measurements from the inserted CD.
 * @param config_device Device path to use in the read_offset section.
 */
void print_offset_measurement(std::ostream& out, const OffsetMeasurement& result, const std::string& config_device);

/** Injectable network lookups used by the measurement command. */
struct OffsetLookupServices {
    DriveOffsetLookup (*drive_offset)(const DriveIdentity&);
    AccurateRipDatabase (*disc_database)(const CdRipDiscToc&);
};

/** Measure the inserted disc and finish without encoding, ejecting, or updating configuration.
 * @param rip Open drive to measure.
 * @param tracks Requested number of tracks.
 * @param services Drive catalogue and disc database lookups.
 * @param device_ids Stable paths for this drive, or an empty list to use the device path.
 * @param out Measurement output stream.
 * @return Zero on a completed measurement, nonzero if it was unavailable or incomplete.
 */
int run_offset_measurement(CdRip* rip, unsigned tracks, const OffsetLookupServices& services,
    const std::vector<std::string>& device_ids, std::ostream& out);
}
