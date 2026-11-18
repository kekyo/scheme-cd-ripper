#include "accuraterip.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <libsoup/soup.h>

namespace cdrip::detail {
void accuraterip_update(AccurateRipChecksum& sum, const int16_t* pcm, size_t frames,
    uint64_t total_frames, bool first, bool last) {
    // AccurateRip excludes disc-edge frames, but retains the original weights.
    // The first included frame has one-based position 2940 (not 2941).
    const uint64_t limit = last ? (total_frames > 2940 ? total_frames - 2940 : 0) : total_frames;
    for (size_t i = 0; i < frames; ++i) {
        const uint64_t position = ++sum.position;
        if ((first && position < 2940) || position > limit) continue;
        const uint32_t packed = static_cast<uint16_t>(pcm[i * 2]) |
            (static_cast<uint32_t>(static_cast<uint16_t>(pcm[i * 2 + 1])) << 16);
        const uint64_t product = static_cast<uint64_t>(packed) * position;
        sum.v1 += static_cast<uint32_t>(product);
        sum.v2 += static_cast<uint32_t>(product) + static_cast<uint32_t>(product >> 32);
    }
}

static std::string hex32(uint32_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(8) << value;
    return out.str();
}

bool accuraterip_disc(const CdRipDiscToc& toc, AccurateRipDisc& disc) {
    disc = {};
    if (toc.has_data_tracks || !toc.tracks || toc.tracks_count == 0 || toc.tracks_count > 99) return false;
    uint32_t digit_sum = 0;
    for (size_t i = 0; i < toc.tracks_count; ++i) {
        const auto& track = toc.tracks[i];
        if (!track.is_audio || track.number != static_cast<int>(i + 1) || track.start < 0 ||
            track.end < track.start || track.end >= INT32_MAX ||
            (i && toc.tracks[i - 1].end + 1 != track.start)) return false;
        disc.id1 += static_cast<uint32_t>(track.start);
        disc.id2 += static_cast<uint32_t>(std::max<long>(1, track.start)) * (i + 1);
        auto seconds = (track.start + 150) / 75;
        while (seconds) { digit_sum += seconds % 10; seconds /= 10; }
    }
    if (toc.leadout_sector != toc.tracks[toc.tracks_count - 1].end + 1) return false;
    disc.tracks = static_cast<unsigned>(toc.tracks_count);
    disc.id1 += static_cast<uint32_t>(toc.leadout_sector);
    disc.id2 += static_cast<uint32_t>(toc.leadout_sector) * (disc.tracks + 1);
    const auto duration = toc.leadout_sector / 75 - toc.tracks[0].start / 75;
    if (duration > 65535) return false;
    // CDDB uses the 150-sector lead-in in digit sums. The existing metadata
    // CDDB identifier is not reused because its offsets have different semantics.
    disc.cddb = ((digit_sum % 255) << 24) | (static_cast<uint32_t>(duration) << 8) | disc.tracks;
    const auto id = hex32(disc.id1);
    std::ostringstream path;
    path << id[7] << '/' << id[6] << '/' << id[5] << "/dBAR-"
         << std::setw(3) << std::setfill('0') << disc.tracks << '-'
         << id << '-' << hex32(disc.id2) << '-' << hex32(disc.cddb) << ".bin";
    disc.path = path.str();
    return true;
}

AccurateRipDatabase accuraterip_response(const AccurateRipDisc& disc, unsigned status,
    const std::vector<uint8_t>& body) {
    AccurateRipDatabase db{};
    if (status == 404) { db.status = AccurateRipStatus::NotFound; return db; }
    if (status != 200) {
        db.status = AccurateRipStatus::NetworkError;
        db.detail = "HTTP " + std::to_string(status);
        return db;
    }
    db.status = AccurateRipStatus::InvalidResponse;
    const size_t record_size = 13 + disc.tracks * 9;
    if (!disc.tracks || disc.tracks > 99 || body.empty() || body.size() > 4 * 1024 * 1024 || body.size() % record_size) return db;
    auto le32 = [&](size_t pos) {
        return static_cast<uint32_t>(body[pos]) | (static_cast<uint32_t>(body[pos+1]) << 8) |
            (static_cast<uint32_t>(body[pos+2]) << 16) | (static_cast<uint32_t>(body[pos+3]) << 24);
    };
    for (size_t pos = 0; pos < body.size(); pos += record_size) {
        if (body[pos] != disc.tracks || le32(pos+1) != disc.id1 ||
            le32(pos+5) != disc.id2 || le32(pos+9) != disc.cddb) {
            db.pressings.clear();
            return db;
        }
        std::vector<AccurateRipTrackEntry> tracks;
        for (unsigned i = 0; i < disc.tracks; ++i) {
            const auto entry = pos + 13 + i * 9;
            tracks.push_back({body[entry], le32(entry+1), le32(entry+5)});
        }
        db.pressings.push_back(std::move(tracks));
    }
    db.status = AccurateRipStatus::Available;
    return db;
}

AccurateRipDatabase accuraterip_lookup(const CdRipDiscToc& toc) {
    AccurateRipDisc disc{};
    if (!accuraterip_disc(toc, disc)) return {};
    auto* session = soup_session_new();
    soup_session_set_timeout(session, 15);
    soup_session_set_user_agent(session, "scheme-cd-ripper");
    const auto url = "https://www.accuraterip.com/accuraterip/" + disc.path;
    auto* message = soup_message_new("GET", url.c_str());
    GError* error = nullptr;
    auto* bytes = soup_session_send_and_read(session, message, nullptr, &error);
    const auto status = soup_message_get_status(message);
    std::vector<uint8_t> body;
    if (bytes) {
        gsize size = 0;
        const auto* data = static_cast<const uint8_t*>(g_bytes_get_data(bytes, &size));
        if (size && size <= 4 * 1024 * 1024) body.assign(data, data + size);
    }
    auto db = accuraterip_response(disc, error ? 0 : status, body);
    if (error) db.detail = error->message;
    g_clear_error(&error);
    if (bytes) g_bytes_unref(bytes);
    g_object_unref(message);
    g_object_unref(session);
    return db;
}

std::string accuraterip_lookup_report(const AccurateRipDatabase& db) {
    switch (db.status) {
        case AccurateRipStatus::NotFound: return "not in database";
        case AccurateRipStatus::NetworkError: return "network error: " + db.detail;
        case AccurateRipStatus::InvalidResponse: return "invalid database response";
        case AccurateRipStatus::Unsupported: return "unsupported disc layout";
        case AccurateRipStatus::Available: break;
    }
    const auto records = db.pressings.size();
    const auto tracks = records ? db.pressings.front().size() : 0;
    return "found " + std::to_string(records) + (records == 1 ? " record for " : " records for ") +
        std::to_string(tracks) + (tracks == 1 ? " track" : " tracks");
}

std::string accuraterip_report(const AccurateRipDatabase& db, size_t track_index,
    const AccurateRipChecksum& sum) {
    if (db.status != AccurateRipStatus::Available) return accuraterip_lookup_report(db);
    unsigned v1 = 0, v2 = 0;
    for (const auto& pressing : db.pressings) {
        if (track_index >= pressing.size()) continue;
        const auto& entry = pressing[track_index];
        if (entry.checksum == sum.v1) v1 = std::max(v1, entry.confidence);
        if (entry.checksum == sum.v2) v2 = std::max(v2, entry.confidence);
    }
    const auto crc = " (v1=" + hex32(sum.v1) + ", v2=" + hex32(sum.v2) + ")";
    if (!v1 && !v2) return "mismatch" + crc;
    std::string result = "match";
    if (v1) result += " v1 confidence=" + std::to_string(v1);
    if (v2) result += " v2 confidence=" + std::to_string(v2);
    return result + crc;
}
}
