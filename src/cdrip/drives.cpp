// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <string>
#include <utility>
#include <vector>

#include "internal.h"

using namespace cdrip::detail;

/* ------------------------------------------------------------------- */
/* Exported API functions */

extern "C" {

CdRipDetectedDriveList* cdrip_detect_cd_drives() {
    CdRipDetectedDriveList* list = new CdRipDetectedDriveList{};
    std::vector<CdRipDetectedDrive> devices;
    const auto backend_devices = current_drive_backend().detect_drives();
    devices.reserve(backend_devices.size());
    for (const auto& backend_drive : backend_devices) {
        CdRipDetectedDrive d{};
        d.device = make_cstr_copy(backend_drive.device);
        d.has_media = backend_drive.has_media;
        devices.push_back(std::move(d));
    }
    if (!devices.empty()) {
        list->count = devices.size();
        list->drives = new CdRipDetectedDrive[list->count]{};
        for (std::size_t i = 0; i < list->count; ++i) {
            list->drives[i] = devices[i];
        }
    }
    return list;
}

void cdrip_release_detecteddrive_list(
    CdRipDetectedDriveList* p) {

    if (!p) return;
    if (p->drives) {
        for (size_t i = 0; i < p->count; ++i) {
            release_cstr(p->drives[i].device);
        }
        delete[] p->drives;
        p->drives = nullptr;
    }
    p->count = 0;
    delete p;
}

};
