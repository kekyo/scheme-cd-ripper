#include "../src/cdrip/accuraterip.h"
#include <cstdlib>
#include <iostream>
using namespace cdrip::detail;
static void expect(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    std::vector<int16_t> pcm(7056 * 2);
    for (size_t i = 0; i < 7056; ++i) {
        pcm[i*2] = i % 2 ? 1 : -1;
        pcm[i*2+1] = i % 2 ? -32768 : -1;
    }
    // Independently evaluated integer sums; exercise high-word folding and edges.
    const uint32_t v1[] = {0xdc8, 0x1386, 0x80a, 0xdc8};
    const uint32_t v2[] = {0x11ce944, 0xeb8219, 0x60f49b, 0x2f8d70};
    for (int flags = 0; flags < 4; ++flags) {
        AccurateRipChecksum sum{};
        accuraterip_update(sum, pcm.data(), 3000, 7056, flags & 1, flags & 2);
        accuraterip_update(sum, pcm.data()+6000, 4056, 7056, flags & 1, flags & 2);
        expect(sum.v1 == v1[flags] && sum.v2 == v2[flags], "v1/v2 checksum or edge exclusion");
    }
    CdRipTrackInfo tracks[] = {{1,0,7499,1}, {2,7500,14999,1}};
    CdRipDiscToc toc{};
    toc.tracks = tracks; toc.tracks_count = 2; toc.leadout_sector = 15000;
    AccurateRipDisc disc{};
    expect(accuraterip_disc(toc, disc), "audio-only TOC supported");
    expect(disc.path == "4/e/7/dBAR-002-000057e4-0000ea61-0500c802.bin", "disc IDs and CDDB lead-in");
    toc.has_data_tracks = true;
    expect(!accuraterip_disc(toc, disc), "mixed mode rejected");
    toc.has_data_tracks = false;
    expect(accuraterip_disc(toc, disc), "restore disc");
    std::vector<uint8_t> body;
    auto put32 = [&](uint32_t x) { for (int i=0;i<4;++i) body.push_back(static_cast<uint8_t>(x >> (8*i))); };
    for (unsigned pressing : {0U,1U}) {
        body.push_back(2); put32(disc.id1); put32(disc.id2); put32(disc.cddb);
        body.push_back(pressing ? 7 : 2); put32(pressing ? 0x5678 : 0x1234); put32(0x89abcdef);
        body.push_back(3); put32(0x9999); put32(0);
    }
    const auto db = accuraterip_response(disc, 200, body);
    expect(db.status == AccurateRipStatus::Available && db.pressings.size() == 2, "multiple pressings");
    expect(accuraterip_lookup_report(db) == "found 2 records for 2 tracks", "report disc records before any audio is read");
    auto single = db;
    single.pressings.resize(1);
    single.pressings.front().resize(1);
    expect(accuraterip_lookup_report(single) == "found 1 record for 1 track", "singular record and track counts");
    expect(accuraterip_lookup_report(accuraterip_response(disc, 404, {})) == "not in database", "report unregistered discs");
    expect(accuraterip_lookup_report(accuraterip_response(disc, 503, {})) == "network error: HTTP 503", "show the network error detail");
    expect(accuraterip_lookup_report(accuraterip_response(disc, 200, {})) == "invalid database response", "report malformed records");
    expect(accuraterip_lookup_report({}) == "unsupported disc layout", "report unsupported discs");
    expect(db.pressings[1][0].offset_checksum == 0x89abcdef, "offset signature must retain its little-endian value");
    expect(accuraterip_report(db, 0, {0x1234,0x5678,7056}).find("v2 confidence=7") != std::string::npos, "match all candidates");
    expect(accuraterip_report(db, 1, {1,2,7056}).find("mismatch") == 0, "mismatch");
    expect(accuraterip_response(disc,404,{}).status == AccurateRipStatus::NotFound, "404 is not registered");
    expect(accuraterip_response(disc,503,{}).status == AccurateRipStatus::NetworkError, "503 is a network failure");
    expect(accuraterip_response(disc,0,{}).status == AccurateRipStatus::NetworkError, "transport failure");
    for (size_t size=0;size<body.size();++size) {
        if (size == 31) continue;
        expect(accuraterip_response(disc,200,{body.begin(),body.begin()+size}).status == AccurateRipStatus::InvalidResponse, "truncation rejected");
    }
    body[32] ^= 1;
    expect(accuraterip_response(disc,200,body).status == AccurateRipStatus::InvalidResponse, "wrong disc ID rejected");
}
