#include "offset_measurement.h"
#include "internal.h"
#include "pcm_reader.h"
#include "settings.h"
#include <algorithm>
#include <iomanip>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <libsoup/soup.h>

namespace cdrip::detail {

static std::string collapse_spaces(const std::string& value) {
    std::istringstream input(normalized_setting(value));
    std::string result, word;
    while (input >> word) { if (!result.empty()) result += ' '; result += word; }
    return result;
}

static std::string catalogue_vendor(const std::string& vendor) {
    const auto normalized = collapse_spaces(vendor);
    // LG optical drives identify themselves using the HL-DT-ST inquiry vendor.
    return normalized == "hl-dt-st" ? "lg electronics" : normalized;
}

static std::string cell_text(const std::string& html) {
    std::string text;
    bool in_tag = false;
    for (char ch : html) {
        if (ch == '<') in_tag = true;
        else if (ch == '>') in_tag = false;
        else if (!in_tag) text += ch;
    }
    for (const auto& entity : {std::pair{"&nbsp;", " "}, {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}}) {
        size_t pos = 0;
        while ((pos = text.find(entity.first, pos)) != std::string::npos) {
            text.replace(pos, std::char_traits<char>::length(entity.first), entity.second);
            pos += std::char_traits<char>::length(entity.second);
        }
    }
    return collapse_spaces(text);
}

DriveOffsetLookup parse_drive_offsets(const std::string& html, const DriveIdentity& identity) {
    if (identity.vendor.empty() || identity.model.empty()) return {{}, "drive identity unavailable"};
    std::string lower = html;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto vendor = catalogue_vendor(identity.vendor);
    const auto model = collapse_spaces(identity.model);
    DriveOffsetLookup result{{}, "not listed"};
    bool has_rows = false, matched = false, unavailable = false;
    size_t pos = 0;
    while ((pos = lower.find("<tr", pos)) != std::string::npos) {
        const auto end = lower.find("</tr>", pos);
        if (end == std::string::npos) break;
        std::vector<std::string> cells;
        size_t cell = pos;
        while ((cell = lower.find("<td", cell)) != std::string::npos && cell < end) {
            const auto begin = lower.find('>', cell);
            const auto limit = lower.find("</td>", begin);
            if (begin == std::string::npos || limit == std::string::npos || limit > end) break;
            cells.push_back(cell_text(html.substr(begin + 1, limit - begin - 1)));
            cell = limit + 5;
        }
        pos = end + 5;
        if (cells.size() < 2) continue;
        const auto separator = cells[0].find(" - ");
        if (separator == std::string::npos) continue;
        has_rows = true;
        if (catalogue_vendor(cells[0].substr(0, separator)) != vendor ||
            collapse_spaces(cells[0].substr(separator + 3)) != model) continue;
        int samples = 0;
        if (!parse_setting_integer(cells[1], samples)) { unavailable = true; continue; }
        if (matched && result.samples != samples) return {{}, "ambiguous catalogue entries"};
        matched = true;
        result = {samples, "listed"};
    }
    if (!has_rows) return {{}, "invalid drive catalogue response"};
    if (unavailable) return {{}, "listed model has no consistent offset"};
    return result;
}

DriveOffsetLookup lookup_drive_offset(const DriveIdentity& identity) {
    if (identity.vendor.empty() || identity.model.empty()) return {{}, "drive identity unavailable"};
    auto* session = soup_session_new();
    soup_session_set_timeout(session, 15);
    soup_session_set_user_agent(session, "scheme-cd-ripper");
    auto* message = soup_message_new("GET", "https://www.accuraterip.com/driveoffsets.htm");
    GError* error = nullptr;
    auto* bytes = soup_session_send_and_read(session, message, nullptr, &error);
    DriveOffsetLookup result{{}, error ? error->message : "drive catalogue request failed"};
    if (!error && bytes && soup_message_get_status(message) == 200) {
        gsize size = 0;
        const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
        if (size && size <= 4 * 1024 * 1024) result = parse_drive_offsets(std::string(data, size), identity);
        else result.detail = "invalid drive catalogue size";
    } else if (!error) result.detail = "HTTP " + std::to_string(soup_message_get_status(message));
    g_clear_error(&error);
    if (bytes) g_bytes_unref(bytes);
    g_object_unref(message);
    g_object_unref(session);
    return result;
}

static constexpr int kSearchLimit = 2940;
static constexpr uint64_t kPcmMemoryLimit = 1024ULL * 1024 * 1024;

struct CapturedTrack {
    int64_t track_first;
    int64_t track_frames;
    int64_t first;
    bool first_track;
    bool last_track;
    std::vector<int16_t> pcm;
};

// Preserve the original one-based weights and the actual disc-edge exclusions.
static std::pair<int64_t, int64_t> checksum_interval(const CapturedTrack& track) {
    return {track.first_track ? 2939 : 0,
        track.last_track ? std::max<int64_t>(0, track.track_frames - 2940) : track.track_frames};
}

static std::optional<AccurateRipChecksum> checksum_at(const CapturedTrack& track, int offset) {
    const auto [begin, end] = checksum_interval(track);
    const int64_t first = track.track_first + begin + offset - track.first;
    if (begin >= end || first < 0 || first + end - begin > static_cast<int64_t>(track.pcm.size() / 2)) return {};
    AccurateRipChecksum sum{};
    sum.position = begin;
    accuraterip_update(sum, track.pcm.data() + first * 2, static_cast<size_t>(end - begin),
        track.track_frames, track.first_track, track.last_track);
    return sum;
}

static void record_matches(OffsetTrackResult& result, const AccurateRipDatabase& database,
    const AccurateRipChecksum& sum, int offset) {
    for (size_t record = 0; record < database.pressings.size(); ++record) {
        if (result.number > database.pressings[record].size()) continue;
        const auto& entry = database.pressings[record][result.number - 1];
        if (!entry.confidence) continue;
        const bool v1 = entry.checksum == sum.v1, v2 = entry.checksum == sum.v2;
        if (v1 || v2) result.matches.push_back({offset, record, entry.confidence, v1, v2});
    }
}

static uint32_t packed_frame(const CapturedTrack& track, int64_t frame) {
    const size_t index = static_cast<size_t>(frame - track.first) * 2;
    return static_cast<uint16_t>(track.pcm[index]) |
        (static_cast<uint32_t>(static_cast<uint16_t>(track.pcm[index + 1])) << 16);
}

static std::optional<uint32_t> offset_signature(const CapturedTrack& track, int offset) {
    // The database's final four entry bytes describe 588 stereo frames starting
    // at CD sector 450, weighted 1..588. This can filter both v1 and v2 records.
    // Format reference: CUETools.AccurateRip/AccurateRip.cs, CRC450(), at
    // https://github.com/gchudov/cuetools.net
    const int64_t first = track.track_first + 450 * 588 + offset;
    if (track.track_frames < 451 * 588 || first < track.first ||
        first + 588 > track.first + static_cast<int64_t>(track.pcm.size() / 2)) return {};
    uint32_t sum = 0;
    for (unsigned i = 0; i < 588; ++i) sum += packed_frame(track, first + i) * (i + 1);
    return sum;
}

static void search_track_offsets(OffsetTrackResult& result, const CapturedTrack& track,
    const AccurateRipDatabase& database, std::optional<int> listed, const MeasurementProgress& progress) {
    if (!result.error.empty()) return;
    const auto [begin, end] = checksum_interval(track);
    if (begin >= end) return;
    const int64_t first = std::max<int64_t>(-kSearchLimit, track.first - track.track_first - begin);
    const int64_t last = std::min<int64_t>(kSearchLimit,
        track.first + static_cast<int64_t>(track.pcm.size() / 2) - track.track_first - end);
    if (first > last) return;
    result.search_range = {static_cast<int>(first), static_cast<int>(last)};

    // Keep both the unweighted sum S and weighted v1 sum W modulo 2^32.
    // Sliding [begin,end) by one frame gives W' = W-S-begin*old+end*next.
    uint32_t sum = 0, weighted = 0;
    for (int64_t i = begin; i < end; ++i) {
        const auto word = packed_frame(track, track.track_first + first + i);
        sum += word;
        weighted += word * static_cast<uint32_t>(i + 1);
    }
    const bool silent = std::all_of(track.pcm.begin(), track.pcm.end(), [](int16_t sample) { return sample == 0; });
    const size_t total = static_cast<size_t>(last - first + 1);
    if (progress) progress("Searching", result.number, 0, total);
    for (int offset = static_cast<int>(first); offset <= last; ++offset) {
        if (offset != listed) {
            const auto signature = offset_signature(track, offset);
            const bool candidate = std::any_of(database.pressings.begin(), database.pressings.end(),
                [&](const auto& record) {
                    if (result.number > record.size()) return false;
                    const auto& entry = record[result.number - 1];
                    // Missing signatures and short tracks require a full v2 check.
                    return entry.confidence && (entry.checksum == weighted || !signature ||
                        !entry.offset_checksum || entry.offset_checksum == *signature);
                });
            if (candidate) {
                const auto checksum = silent ? AccurateRipChecksum{} : *checksum_at(track, offset);
                record_matches(result, database, checksum, offset);
            }
        }
        if (offset < last) {
            const auto old_word = packed_frame(track, track.track_first + offset + begin);
            const auto next_word = packed_frame(track, track.track_first + offset + end);
            weighted = weighted - sum - static_cast<uint32_t>(begin) * old_word + static_cast<uint32_t>(end) * next_word;
            sum = sum - old_word + next_word;
        }
        const size_t done = static_cast<size_t>(offset - first + 1);
        if (progress && (done % 64 == 0 || done == total)) progress("Searching", result.number, done, total);
    }
    std::sort(result.matches.begin(), result.matches.end(), [](const auto& a, const auto& b) {
        return std::pair{a.samples, a.record} < std::pair{b.samples, b.record};
    });
}

OffsetMeasurement measure_offsets(CdRip* rip, const CdRipDiscToc& toc,
    const AccurateRipDatabase& database, std::optional<int> listed_offset,
    unsigned tracks, const MeasurementProgress& progress) {
    OffsetMeasurement result{tracks, listed_offset, false, 0, 0, {}, {}};
    AccurateRipDisc disc{};
    if (!tracks || !accuraterip_disc(toc, disc)) { result.error = "unsupported disc layout or track count"; return result; }
    if (database.status != AccurateRipStatus::Available) {
        result.error = accuraterip_report(database, 0, {});
        return result;
    }
    const size_t count = std::min<size_t>(tracks, toc.tracks_count);
    const int64_t audio_first = static_cast<int64_t>(toc.tracks[0].start) * 588;
    const int64_t audio_limit = static_cast<int64_t>(toc.leadout_sector) * 588;
    std::vector<CapturedTrack> captured;
    uint64_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& info = toc.tracks[i];
        result.tracks.push_back({static_cast<unsigned>(info.number), {}, {}, {}});
        auto& measured = result.tracks.back();
        const int64_t start = static_cast<int64_t>(info.start) * 588;
        const int64_t frames = (static_cast<int64_t>(info.end) - info.start + 1) * 588;
        const int64_t first = std::max(audio_first, start + std::min(-kSearchLimit, listed_offset.value_or(0)));
        const int64_t limit = std::min(audio_limit, start + frames + std::max(kSearchLimit, listed_offset.value_or(0)));
        const uint64_t size = static_cast<uint64_t>(limit - first);
        if (size > (kPcmMemoryLimit - bytes) / 4) { result.error = "measurement PCM exceeds 1 GiB memory limit"; return result; }
        bytes += size * 4;
        captured.push_back({start, frames, first, i == 0, i + 1 == toc.tracks_count, {}});
        auto& buffer = captured.back();
        try { buffer.pcm.resize(static_cast<size_t>(size) * 2); }
        catch (const std::bad_alloc&) { result.error = "not enough memory for measurement PCM"; return result; }
        PcmReadState reader{rip};
        for (size_t done = 0; done < size;) {
            const size_t chunk = std::min<size_t>(size - done, 128 * 588);
            if (!read_pcm_frames(reader, first + static_cast<int64_t>(done), chunk, audio_first,
                    audio_limit, buffer.pcm.data() + done * 2, measured.error)) break;
            done += chunk;
            if (progress) progress("Reading", measured.number, done, size);
        }
        if (!measured.error.empty()) { buffer.pcm.clear(); continue; }
        if (listed_offset) {
            measured.listed_checksum = checksum_at(buffer, *listed_offset);
            if (measured.listed_checksum) record_matches(measured, database, *measured.listed_checksum, *listed_offset);
        }
    }
    // A shared record must cover every selected track before a listed value can
    // skip searching; unrelated per-track matches are insufficient.
    const bool listed_match = std::any_of(result.tracks[0].matches.begin(), result.tracks[0].matches.end(),
        [&](const auto& match) {
            return std::all_of(result.tracks.begin(), result.tracks.end(), [&](const auto& track) {
                return std::any_of(track.matches.begin(), track.matches.end(),
                    [&](const auto& other) { return other.record == match.record; });
            });
        });
    if (!listed_match) {
        result.searched = true;
        result.search_min = -kSearchLimit;
        result.search_max = kSearchLimit;
        for (size_t i = 0; i < count; ++i) search_track_offsets(result.tracks[i], captured[i], database, listed_offset, progress);
    }
    return result;
}

static std::string signed_samples(int samples) {
    return (samples >= 0 ? "+" : "") + std::to_string(samples);
}

void print_offset_measurement(std::ostream& out, const OffsetMeasurement& result) {
    out << "Requested tracks: " << result.requested_tracks << "; measured tracks: " << result.tracks.size() << '\n';
    if (!result.error.empty()) out << "Measurement unavailable: " << result.error << '\n';
    if (result.searched) out << "Offset search bounds: " << signed_samples(result.search_min) << ".." << signed_samples(result.search_max) << " stereo samples\n";
    std::map<std::pair<int, size_t>, size_t> common;
    for (const auto& track : result.tracks) {
        out << "Track " << track.number << ":\n";
        if (!track.error.empty()) { out << "  Read error: " << track.error << '\n'; continue; }
        if (result.searched) {
            if (track.search_range) out << "  Searched offsets: " << signed_samples(track.search_range->first)
                << ".." << signed_samples(track.search_range->second) << '\n';
            else out << "  Search unavailable: required audio missing\n";
        }
        if (result.listed_offset) {
            out << "  Listed offset " << signed_samples(*result.listed_offset) << ": ";
            if (track.listed_checksum) {
                out << "v1=" << std::hex << std::setw(8) << std::setfill('0') << track.listed_checksum->v1
                    << " v2=" << std::setw(8) << track.listed_checksum->v2 << std::dec << std::setfill(' ') << '\n';
            } else out << "required audio unavailable\n";
        }
        if (track.matches.empty()) out << "  No matching offset in the tested range\n";
        for (const auto& match : track.matches) {
            out << "  Offset " << signed_samples(match.samples) << ": match "
                << (match.v1 ? "v1 " : "") << (match.v2 ? "v2 " : "")
                << "confidence=" << match.confidence << " record=" << match.record + 1 << '\n';
            ++common[{match.samples, match.record}];
        }
    }
    bool full_match = false;
    for (const auto& [key, count] : common) {
        if (count != result.tracks.size()) continue;
        full_match = true;
        out << "Offset " << signed_samples(key.first) << ": all " << count
            << " measured tracks match record " << key.second + 1 << '\n';
    }
    if (!full_match && !result.tracks.empty()) out << "No single database record matches all measured tracks at one offset\n";
}

int run_offset_measurement(CdRip* rip, unsigned tracks, const OffsetLookupServices& services, std::ostream& out) {
    const char* error = nullptr;
    std::unique_ptr<CdRipDiscToc, decltype(&cdrip_release_disctoc)> toc(
        cdrip_build_disc_toc(rip, &error), &cdrip_release_disctoc);
    if (!toc) {
        out << "Measurement unavailable: " << to_string_or_empty(error) << '\n';
        cdrip_release_error(error);
        return 1;
    }
    cdrip_release_error(error);
    AccurateRipDisc disc{};
    if (!accuraterip_disc(*toc, disc)) { out << "Measurement unavailable: unsupported disc layout\n"; return 1; }
    DriveIdentity identity;
    std::string identity_error;
    if (rip->backend && rip->backend->get_identity) rip->backend->get_identity(rip->drive, identity, identity_error);
    out << "Drive: " << identity.vendor << ' ' << identity.model << " revision=" << identity.revision << '\n';
    if (!identity_error.empty()) out << "Drive identity: " << identity_error << '\n';
    out << "AccurateRip disc: " << disc.path << "\nFetching drive offset catalogue...\n" << std::flush;
    const auto listed = services.drive_offset(identity);
    out << "Catalogue: " << listed.detail;
    if (listed.samples) out << " (" << signed_samples(*listed.samples) << " stereo samples)";
    out << "\nFetching AccurateRip disc records...\n" << std::flush;
    const auto database = services.disc_database(*toc);
    const auto result = measure_offsets(rip, *toc, database, listed.samples, tracks,
        [&out](const char* phase, unsigned track, size_t done, size_t total) {
            out << '\r' << phase << " track " << track << ": " << done * 100 / total << "%" << std::flush;
            if (done == total) out << '\n';
        });
    out << '\n';
    print_offset_measurement(out, result);
    if (!result.error.empty()) return 1;
    return std::all_of(result.tracks.begin(), result.tracks.end(),
        [](const auto& track) { return track.error.empty() && (track.listed_checksum || track.search_range); }) ? 0 : 1;
}
}
