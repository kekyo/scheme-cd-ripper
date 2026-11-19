#include "pcm_reader.h"
#include "internal.h"
#include <algorithm>

namespace cdrip::detail {
bool seek_pcm_frame(PcmReadState& state, int64_t first, std::string& error) {
    state.audio = nullptr;
    state.sector = state.next_sector = -1;
    if (!state.rip || !state.rip->reader || first < 0) { error = "Invalid reader or source frame"; return false; }
    const auto& backend = state.rip->backend ? *state.rip->backend : current_drive_backend();
    const auto sector = static_cast<long>(first / 588);
    if (!backend.seek_reader(state.rip->reader, sector, error)) return false;
    state.next_sector = sector;
    return true;
}

bool read_pcm_frames(PcmReadState& state, int64_t first, size_t frames,
    int64_t audio_first, int64_t audio_limit, int16_t* output, std::string& error) {
    error.clear();
    if (!state.rip || !state.rip->reader) { error = "Reader handle is null"; return false; }
    const auto& backend = state.rip->backend ? *state.rip->backend : current_drive_backend();
    size_t copied = 0;
    while (copied < frames) {
        const auto position = first + static_cast<int64_t>(copied);
        if (position < audio_first || position >= audio_limit) {
            const size_t count = position < audio_first
                ? std::min<uint64_t>(frames - copied, audio_first - position) : frames - copied;
            std::fill_n(output + copied * 2, count * 2, int16_t{0});
            copied += count;
            continue;
        }
        const auto sector = static_cast<long>(position / 588);
        if (sector != state.sector) {
            if (sector != state.next_sector && !seek_pcm_frame(state, position, error)) {
                return false;
            }
            if (!backend.read_sector(state.rip->reader, state.audio, error) || !state.audio) {
                state.audio = nullptr;
                state.next_sector = -1;
                error = "source sector " + std::to_string(sector) + ": " + error;
                return false;
            }
            state.sector = sector;
            state.next_sector = sector + 1;
        }
        const auto index = static_cast<size_t>(position % 588);
        const size_t count = std::min<uint64_t>(std::min(frames - copied, 588 - index), audio_limit - position);
        std::copy_n(state.audio + index * 2, count * 2, output + copied * 2);
        copied += count;
    }
    return true;
}
}
