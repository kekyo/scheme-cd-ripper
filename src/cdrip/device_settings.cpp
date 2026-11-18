#include "device_settings.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace cdrip::detail {
std::string canonicalize_device_path(const std::string& path) {
    if (path.empty()) return {};
    // realpath() resolves symlinks (e.g., /dev/cdrom -> /dev/sr0).
    // If it fails (non-path style device name, permission, etc.), fall back to the original string.
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) return path;
    std::string out = resolved;
    std::free(resolved);
    return out;
}

std::vector<std::string> device_id_paths(const std::string& device, const std::string& directory) {
    std::vector<std::string> paths;
    std::error_code error;
    const auto target = std::filesystem::canonical(device, error);
    if (error) return paths;
    std::filesystem::directory_iterator it(directory, error), end;
    while (!error && it != end) {
        std::error_code entry_error;
        if (it->is_symlink(entry_error)) {
            const auto resolved = std::filesystem::canonical(it->path(), entry_error);
            if (!entry_error && resolved == target) paths.push_back(it->path().string());
        }
        it.increment(error);
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

ReadOffsetSelection select_read_offset(const CdRipConfig& config, const std::string& device,
    std::optional<int> explicit_offset) {
    if (explicit_offset) return {*explicit_offset, "CLI", {}};
    ReadOffsetSelection result;
    const auto target = canonicalize_device_path(device);
    bool found = false;
    for (size_t i = 0; i < config.read_offsets_count; ++i) {
        const auto& entry = config.read_offsets[i];
        if (canonicalize_device_path(entry.device) != target) continue;
        if (found && result.samples != entry.samples) {
            result.error = "Conflicting read offsets for " + device + ": " + result.source + " and " + entry.device;
            return result;
        }
        found = true;
        result.samples = entry.samples;
        result.source = entry.device;
    }
    return result;
}
}
