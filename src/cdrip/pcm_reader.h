#pragma once
#include <cstdint>
#include <string>
#include "cdrip/cdrip.h"

namespace cdrip::detail {
/** Reader-local sector cache. The backend owns audio until the next sector read. */
struct PcmReadState {
    CdRip* rip;
    long sector{-1};
    const int16_t* audio{nullptr};
    long next_sector{-1};
};

/** Position the backend before reading and invalidate any cached audio. */
bool seek_pcm_frame(PcmReadState& state, int64_t first, std::string& error);

/** Read absolute stereo frames, zero-filling outside the supplied audio interval.
 * @param state Cache for this reader; do not seek the backend outside this state.
 * @param first Absolute first frame to read.
 * @param frames Number of stereo frames to copy to output.
 * @param audio_first First readable audio frame.
 * @param audio_limit Exclusive end of readable audio.
 * @param output Caller-owned interleaved PCM, with room for frames * 2 samples.
 * @param error Failure detail including the source sector.
 * @return Whether all requested readable audio was obtained.
 */
bool read_pcm_frames(PcmReadState& state, int64_t first, size_t frames,
    int64_t audio_first, int64_t audio_limit, int16_t* output, std::string& error);
}
