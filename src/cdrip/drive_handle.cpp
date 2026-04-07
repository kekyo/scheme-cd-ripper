// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <stdint.h>

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
    const DriveBackend& backend = current_drive_backend();
    void* raw = nullptr;
    std::string backend_err;
    if (!backend.open_drive(device_str, raw, backend_err)) {
        set_error(error, backend_err);
        return nullptr;
    }

    if (!backend.set_drive_speed(raw, speed_fast, backend_err)) {
        backend.close_drive(raw);
        set_error(error, backend_err);
        return nullptr;
    }

    const CdRipRipModes effective_mode = (mode == RIP_MODES_DEFAULT)
        ? RIP_MODES_BEST : mode;
    void* reader = nullptr;
    if (!backend.create_reader(raw, effective_mode, reader, backend_err)) {
        backend.close_drive(raw);
        set_error(error, backend_err);
        return nullptr;
    }
    return new CdRip{&backend, raw, reader, effective_mode, device_str, format, compression_level, speed_fast};
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
    const DriveBackend& backend =
        cdrip->backend ? *cdrip->backend : current_drive_backend();
    if (cdrip->reader) {
        backend.destroy_reader(cdrip->reader);
        cdrip->reader = nullptr;
    }
    if (cdrip->drive) {
        backend.close_drive(cdrip->drive);
        cdrip->drive = nullptr;
    }
    if (will_eject && !device.empty()) {
        std::string backend_err;
        if (!backend.eject_drive(device, backend_err)) {
            set_error(error, backend_err);
        }
    }
    delete cdrip;
}

};
