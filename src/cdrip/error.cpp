// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include "internal.h"

/* ------------------------------------------------------------------- */
/* Exported API functions */

extern "C" {

void cdrip_release_error(
    const char* p) {
    
    delete[] p;
}

};
