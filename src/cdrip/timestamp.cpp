// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <string>
#include <sstream>
#include <cstring>

#include "cdrip/cdrip.h"

/* ------------------------------------------------------------------- */
/* Exported API functions */

extern "C" {

char* cdrip_current_timestamp_iso() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const std::tm local_tm = *std::localtime(&t);
    int offset_minutes = 0;
#if defined(__unix__) || defined(__APPLE__)
    long gmtoff = 0;
#if defined(__GLIBC__) || defined(__APPLE__)
    gmtoff = local_tm.tm_gmtoff;
#endif
    if (gmtoff == 0) {
        // Fallback: compute offset as difference between local and UTC interpreted as local
        std::tm utc_tm = *std::gmtime(&t);
        std::tm local_copy = local_tm;
        std::time_t local_as_time = std::mktime(&local_copy);
        std::time_t utc_as_local = std::mktime(&utc_tm);
        gmtoff = static_cast<long>(
            std::difftime(local_as_time, utc_as_local));
    }
    offset_minutes = static_cast<int>(gmtoff / 60);
#endif
    const int offset_hours = offset_minutes / 60;
    const int offset_min = std::abs(offset_minutes % 60);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%dT%H:%M:%S");
    oss << (offset_minutes >= 0 ? "+" : "-")
        << std::setw(2) << std::setfill('0') << std::abs(offset_hours) << ":"
        << std::setw(2) << std::setfill('0') << offset_min;
    const std::string ts = oss.str();
    char* buf = new char[ts.size() + 1];
    std::memcpy(buf, ts.c_str(), ts.size() + 1);
    return buf;
}

void cdrip_release_timestamp(char* p) {
    delete[] p;
}

};
