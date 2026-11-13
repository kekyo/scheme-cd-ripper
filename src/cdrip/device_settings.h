#pragma once
#include <optional>
#include <string>
#include <vector>
#include "cdrip/cdrip.h"

namespace cdrip::detail {
/** Resolve device symlinks, retaining the original path when unavailable. */
std::string canonicalize_device_path(const std::string& path);

/** List sorted stable symlinks in directory that identify the selected device. */
std::vector<std::string> device_id_paths(const std::string& device, const std::string& directory);

/** A resolved offset and its origin, or a conflicting-configuration error. */
struct ReadOffsetSelection {
    int samples{0};
    std::string source{"default"};
    std::string error;
};

/** Resolve CLI, matching device entries, then zero; an explicit zero overrides configuration. */
ReadOffsetSelection select_read_offset(const CdRipConfig& config, const std::string& device,
    std::optional<int> explicit_offset);
}
