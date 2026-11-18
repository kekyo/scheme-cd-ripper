#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "cdrip/cdrip.h"

namespace cdrip::detail {
// Streaming sums over packed stereo frames, with one-based track positions.
struct AccurateRipChecksum {
    uint32_t v1{0};
    uint32_t v2{0};
    uint64_t position{0};
};
struct AccurateRipDisc {
    unsigned tracks{0};
    uint32_t id1{0};
    uint32_t id2{0};
    uint32_t cddb{0};
    std::string path{};
};
enum class AccurateRipStatus { Available, NotFound, NetworkError, InvalidResponse, Unsupported };
struct AccurateRipTrackEntry {
    unsigned confidence{0};
    uint32_t checksum{0};
    /** Weighted checksum of CD sector 450 within the track; zero may mean absent. */
    uint32_t offset_checksum{0};
};
struct AccurateRipDatabase {
    AccurateRipStatus status{AccurateRipStatus::Unsupported};
    std::string detail{};
    std::vector<std::vector<AccurateRipTrackEntry>> pressings{};
};
/** Add corrected interleaved stereo PCM to a track checksum.
 * @param sum Running checksum and frame position.
 * @param pcm Signed 16-bit interleaved PCM.
 * @param frames Number of stereo frames in this block.
 * @param total_frames Complete track length in frames.
 * @param first Whether this is the first disc track.
 * @param last Whether this is the last disc track.
 */
void accuraterip_update(AccurateRipChecksum& sum, const int16_t* pcm, size_t frames,
    uint64_t total_frames, bool first, bool last);
/** Build identifiers for a contiguous, audio-only disc.
 * @param toc Original disc TOC, including the mixed-mode marker.
 * @param disc Output identifiers and relative lookup path.
 * @return False for unsupported or invalid layouts.
 */
bool accuraterip_disc(const CdRipDiscToc& toc, AccurateRipDisc& disc);
/** Interpret an HTTP response, validating every pressing before accepting it.
 * @param disc Expected identifiers.
 * @param status HTTP status (zero on transport failure).
 * @param body Binary response.
 * @return Database records or a distinct lookup failure status.
 */
AccurateRipDatabase accuraterip_response(const AccurateRipDisc& disc, unsigned status,
    const std::vector<uint8_t>& body);
/** Fetch database records over HTTPS.
 * @param toc Disc TOC.
 * @return Parsed records, or the lookup failure reason.
 */
AccurateRipDatabase accuraterip_lookup(const CdRipDiscToc& toc);
/** Format the result for one successfully saved track.
 * @param db Database lookup result.
 * @param track_index Zero-based track index.
 * @param sum Checksums of the saved audio.
 * @return Match versions and confidence, or a distinct failure description.
 */
std::string accuraterip_report(const AccurateRipDatabase& db, size_t track_index,
    const AccurateRipChecksum& sum);
}
