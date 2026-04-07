// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <cdio/cdio.h>
#include <cdio/cd_types.h>
#include <cdio/device.h>
#include <cdio/paranoia/cdda.h>
#include <cdio/paranoia/paranoia.h>

#include <string>
#include <vector>

#include "internal.h"

namespace {

using cdrip::detail::BackendDetectedDrive;
using cdrip::detail::DriveBackend;

auto live_detect_drives = []() {
    std::vector<BackendDetectedDrive> devices;
    const driver_id_t driver = DRIVER_DEVICE;
    char** device_list = cdio_get_devices(driver);
    if (!device_list) return devices;

    for (char** p = device_list; *p != nullptr; ++p) {
        BackendDetectedDrive drive{};
        drive.device = *p;
        CdIo_t* cdio = cdio_open(drive.device.c_str(), driver);
        if (cdio) {
            const discmode_t mode = cdio_get_discmode(cdio);
            drive.has_media =
                mode != CDIO_DISC_MODE_NO_INFO &&
                mode != CDIO_DISC_MODE_ERROR;
            cdio_destroy(cdio);
        }
        devices.push_back(std::move(drive));
    }
    cdio_free_device_list(device_list);
    return devices;
};

auto live_open_drive = [](
    const std::string& device,
    void*& out_drive,
    std::string& err) {

    err.clear();
    out_drive = nullptr;
    cdrom_drive_t* raw = cdda_identify(device.c_str(), 1, nullptr);
    if (!raw) {
        err = "Could not open drive " + device;
        return false;
    }
    if (cdda_open(raw) != 0) {
        err = "Failed to access drive " + device;
        cdda_close(raw);
        return false;
    }
    out_drive = raw;
    return true;
};

auto live_close_drive = [](
    void* drive) {

    if (!drive) return;
    cdda_close(static_cast<cdrom_drive_t*>(drive));
};

auto live_set_drive_speed = [](
    void* drive,
    bool speed_fast,
    std::string& err) {

    err.clear();
    if (!drive) {
        err = "Drive handle is null";
        return false;
    }
    // Ignore backend return value to preserve existing behavior.
    cdda_speed_set(static_cast<cdrom_drive_t*>(drive), speed_fast ? 0 : 1);
    return true;
};

auto live_create_reader = [](
    void* drive,
    CdRipRipModes mode,
    void*& out_reader,
    std::string& err) {

    err.clear();
    out_reader = nullptr;
    if (!drive) {
        err = "Drive handle is null";
        return false;
    }

    cdrom_paranoia* reader = paranoia_init(static_cast<cdrom_drive_t*>(drive));
    if (!reader) {
        err = "Failed to initialise cd-paranoia";
        return false;
    }

    const CdRipRipModes effective_mode = (mode == RIP_MODES_DEFAULT)
        ? RIP_MODES_BEST
        : mode;
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
    paranoia_modeset(reader, flags);
    out_reader = reader;
    return true;
};

auto live_destroy_reader = [](
    void* reader) {

    if (!reader) return;
    paranoia_free(static_cast<cdrom_paranoia*>(reader));
};

auto live_eject_drive = [](
    const std::string& device,
    std::string& err) {

    err.clear();
    const driver_return_code_t rc = cdio_eject_media_drive(device.c_str());
    if (rc != DRIVER_OP_SUCCESS) {
        err = "Failed to eject disc from " + device;
        return false;
    }
    return true;
};

auto live_get_track_count = [](
    void* drive,
    int& out_track_count,
    std::string& err) {

    err.clear();
    out_track_count = 0;
    if (!drive) {
        err = "Drive handle is null";
        return false;
    }
    out_track_count = cdda_tracks(static_cast<cdrom_drive_t*>(drive));
    if (out_track_count <= 0) {
        err = "No tracks found on disc";
        return false;
    }
    return true;
};

auto live_get_track_info = [](
    void* drive,
    int track_number,
    CdRipTrackInfo& out_track,
    std::string& err) {

    err.clear();
    out_track = CdRipTrackInfo{};
    if (!drive) {
        err = "Drive handle is null";
        return false;
    }
    cdrom_drive_t* raw = static_cast<cdrom_drive_t*>(drive);
    out_track.start = cdda_track_firstsector(raw, track_number);
    out_track.end = cdda_track_lastsector(raw, track_number);
    out_track.is_audio = cdda_track_audiop(raw, track_number);
    return true;
};

auto live_get_disc_last_sector = [](
    void* drive,
    long& out_last_sector,
    std::string& err) {

    err.clear();
    out_last_sector = 0;
    if (!drive) {
        err = "Drive handle is null";
        return false;
    }
    out_last_sector = cdda_disc_lastsector(static_cast<cdrom_drive_t*>(drive));
    if (out_last_sector < 0) {
        err = "Failed to read disc last sector";
        return false;
    }
    return true;
};

auto live_seek_reader = [](
    void* reader,
    long sector,
    std::string& err) {

    err.clear();
    if (!reader) {
        err = "Reader handle is null";
        return false;
    }
    paranoia_seek(static_cast<cdrom_paranoia*>(reader), sector, SEEK_SET);
    return true;
};

auto live_read_sector = [](
    void* reader,
    const int16_t*& out_buffer,
    std::string& err) {

    err.clear();
    out_buffer = nullptr;
    if (!reader) {
        err = "Reader handle is null";
        return false;
    }
    out_buffer = paranoia_read(static_cast<cdrom_paranoia*>(reader), nullptr);
    if (!out_buffer) {
        err = "Failed to read audio sector";
        return false;
    }
    return true;
};

const DriveBackend kLiveDriveBackend{
    live_detect_drives,
    live_open_drive,
    live_close_drive,
    live_set_drive_speed,
    live_create_reader,
    live_destroy_reader,
    live_eject_drive,
    live_get_track_count,
    live_get_track_info,
    live_get_disc_last_sector,
    live_seek_reader,
    live_read_sector,
};

const DriveBackend* g_override_drive_backend = nullptr;

}  // namespace

namespace cdrip::detail {

const DriveBackend& current_drive_backend() {
    return g_override_drive_backend ? *g_override_drive_backend : kLiveDriveBackend;
}

void set_drive_backend_for_tests(
    const DriveBackend* backend) {

    g_override_drive_backend = backend;
}

void reset_drive_backend_for_tests() {
    g_override_drive_backend = nullptr;
}

}  // namespace cdrip::detail
