// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

#include "internal.h"

namespace {

constexpr double kReplayGainReferenceLufs = -18.0;

std::string format_gain_db(
    double gain_db) {

    std::ostringstream oss;
    oss << std::showpos << std::fixed << std::setprecision(2) << gain_db << " dB";
    return oss.str();
}

std::string format_peak_value(
    double peak) {

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8) << peak;
    return oss.str();
}

}

namespace cdrip::detail {

bool finalize_replaygain_scan(
    ebur128_state* state,
    ReplayGainScanResult& out,
    std::string& err) {

    err.clear();
    out = ReplayGainScanResult{};
    if (!state) {
        err = "ReplayGain state is null";
        return false;
    }

    double loudness = 0.0;
    if (ebur128_loudness_global(state, &loudness) != EBUR128_SUCCESS || !std::isfinite(loudness)) {
        err = "Failed to calculate ReplayGain loudness";
        return false;
    }

    double peak = 0.0;
    for (unsigned int channel = 0; channel < state->channels; ++channel) {
        double channel_peak = 0.0;
        if (ebur128_sample_peak(state, channel, &channel_peak) != EBUR128_SUCCESS ||
            !std::isfinite(channel_peak)) {
            err = "Failed to calculate ReplayGain peak";
            return false;
        }
        peak = std::max(peak, channel_peak);
    }

    out.loudness_lufs = loudness;
    out.peak = peak;
    out.loudness_ok = true;
    out.peak_ok = true;
    return true;
}

std::map<std::string, std::string> build_replaygain_tags(
    const ReplayGainScanResult& track,
    const ReplayGainScanResult& album) {

    std::map<std::string, std::string> tags;
    if (track.loudness_ok) {
        tags["REPLAYGAIN_TRACK_GAIN"] = format_gain_db(
            kReplayGainReferenceLufs - track.loudness_lufs);
    }
    if (track.peak_ok) {
        tags["REPLAYGAIN_TRACK_PEAK"] = format_peak_value(track.peak);
    }
    if (album.loudness_ok) {
        tags["REPLAYGAIN_ALBUM_GAIN"] = format_gain_db(
            kReplayGainReferenceLufs - album.loudness_lufs);
    }
    if (album.peak_ok) {
        tags["REPLAYGAIN_ALBUM_PEAK"] = format_peak_value(album.peak);
    }
    return tags;
}

}
