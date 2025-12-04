// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <cdio/cdio.h>
#include <cdio/cd_types.h>
#include <cdio/device.h>

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
    const driver_id_t driver = DRIVER_DEVICE;
    char** device_list = cdio_get_devices(driver);
    if (device_list) {
        for (char** p = device_list; *p != nullptr; ++p) {
            CdRipDetectedDrive d{};
            d.device = make_cstr_copy(*p);
            const std::string device_str = to_string_or_empty(d.device);
            CdIo_t* cdio = cdio_open(device_str.c_str(), driver);
            if (cdio) {
                discmode_t mode = cdio_get_discmode(cdio);
                if (mode != CDIO_DISC_MODE_NO_INFO &&
                    mode != CDIO_DISC_MODE_ERROR) {
                    d.has_media = true;
                }
                cdio_destroy(cdio);
            }
            devices.push_back(std::move(d));
        }
        cdio_free_device_list(device_list);
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
