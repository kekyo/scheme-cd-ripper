// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <stdint.h>

#include <cdio/cdio.h>
#include <cdio/device.h>

#include "internal.h"

using namespace cdrip::detail;

/* ------------------------------------------------------------------- */
/* Exported API functions */

extern "C" {

CdRip* cdrip_open(
    const char* device,
    const CdRipSettings* settings,
    const char** error) {

    clear_error(error);
    const std::string device_str = to_string_or_empty(device);
    CdRipRipModes mode = settings ? settings->mode : RIP_MODES_DEFAULT;
    const bool speed_fast = settings ? settings->speed_fast : false;
    const std::string format = settings && settings->format
        ? settings->format : std::string{};
    const int compression_level = settings ? settings->compression_level : -1;
    cdrom_drive_t* raw = cdda_identify(device_str.c_str(), 1, nullptr);
    if (!raw) {
        set_error(error, "Could not open drive " + device_str);
        return nullptr;
    }
    if (cdda_open(raw) != 0) {
        set_error(error, "Failed to access drive " + device_str);
        cdda_close(raw);
        return nullptr;
    }
    // Request rip speed (1 => 1x, 0 => rip maximum).
    // Ignore errors; not all drives support it.
    cdda_speed_set(raw, speed_fast ? 0 : 1);
    cdrom_paranoia* p = paranoia_init(raw);
    if (!p) {
        set_error(error, "Failed to initialise cd-paranoia");
        cdda_close(raw);
        return nullptr;
    }
    const CdRipRipModes effective_mode = (mode == RIP_MODES_DEFAULT)
        ? RIP_MODES_BEST : mode;
    int flags = PARANOIA_MODE_DISABLE;
    switch (effective_mode) {
        case RIP_MODES_FAST:
            flags = PARANOIA_MODE_DISABLE;
            break;
        case RIP_MODES_BEST:
            flags = PARANOIA_MODE_FULL;
            break;
        default:
            flags = PARANOIA_MODE_FULL;
            break;
    }
    paranoia_modeset(p, flags);
    return new CdRip{raw, p, effective_mode, device_str, format, compression_level, speed_fast};
}

void cdrip_close(
    CdRip* cdrip,
    bool will_eject,
    const char** error) {

    clear_error(error);
    if (!cdrip) {
        set_error(error, "Drive handle is null");
        return;
    }
    const std::string device = cdrip->device;
    if (cdrip->paranoia) {
        paranoia_free(cdrip->paranoia);
        cdrip->paranoia = nullptr;
    }
    if (cdrip->drive) {
        cdda_close(cdrip->drive);
        cdrip->drive = nullptr;
    }
    if (will_eject && !device.empty()) {
        driver_return_code_t rc = cdio_eject_media_drive(device.c_str());
        if (rc != DRIVER_OP_SUCCESS) {
            set_error(error, "Failed to eject disc from " + device);
        }
    }
    delete cdrip;
}

};
