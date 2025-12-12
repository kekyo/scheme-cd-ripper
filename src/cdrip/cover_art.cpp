// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <glib.h>
#include <libsoup/soup.h>

#include "internal.h"
#include "version.h"

using namespace cdrip::detail;

namespace {

constexpr int kCoverArtTimeoutSec = 15;
constexpr int kCoverArtRetryDelayMs = 1200;

static std::string cover_art_user_agent() {
    std::string ua = "SchemeCDRipper/";
    ua += VERSION;
    ua += " (https://github.com/kekyo/scheme-cd-ripper)";
    return ua;
}

static bool http_get_bytes(
    const std::string& url,
    std::vector<uint8_t>& body,
    std::string& content_type,
    std::string& err) {

    SoupSession* session = soup_session_new();
    if (!session) {
        err = "Failed to create SoupSession for cover art";
        return false;
    }
    std::string ua = cover_art_user_agent();
    g_object_set(
        session,
        "user-agent",
        ua.c_str(),
        "timeout",
        kCoverArtTimeoutSec,
        nullptr);

    bool ok = false;
    std::string current_url = url;
    for (int attempt = 0; attempt < 3; ++attempt) {
        SoupMessage* msg = soup_message_new("GET", current_url.c_str());
        if (!msg) {
            err = "Failed to create SoupMessage for cover art";
            break;
        }
        soup_message_headers_replace(
            soup_message_get_request_headers(msg),
            "Accept",
            "image/*");

        GError* gerr = nullptr;
        GBytes* bytes = soup_session_send_and_read(session, msg, nullptr, &gerr);
        const guint status = soup_message_get_status(msg);
        if (SOUP_STATUS_IS_REDIRECTION(status)) {
            const char* loc = soup_message_headers_get_one(
                soup_message_get_response_headers(msg),
                "Location");
            if (loc && attempt < 2) {
                current_url = loc;
                g_clear_error(&gerr);
                g_object_unref(msg);
                if (bytes) g_bytes_unref(bytes);
                continue;
            }
        }
        if (status == 429 && attempt == 0) {
            g_clear_error(&gerr);
            g_object_unref(msg);
            if (bytes) g_bytes_unref(bytes);
            std::this_thread::sleep_for(std::chrono::milliseconds(kCoverArtRetryDelayMs));
            continue;
        }
        if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
            std::string resp_body;
            if (bytes) {
                gsize elen = 0;
                const gchar* edata = static_cast<const gchar*>(g_bytes_get_data(bytes, &elen));
                if (edata && elen > 0) resp_body.assign(edata, elen);
            }
            std::ostringstream oss;
            oss << "Cover Art Archive request failed with status " << status;
            if (gerr && gerr->message) {
                oss << ": " << gerr->message;
            }
            if (!resp_body.empty()) {
                oss << " (" << resp_body << ")";
            }
            err = oss.str();
            g_clear_error(&gerr);
            g_object_unref(msg);
            if (bytes) g_bytes_unref(bytes);
            break;
        }

        if (!bytes) {
            err = "Cover Art Archive response body is empty";
            g_clear_error(&gerr);
            g_object_unref(msg);
            break;
        }

        gsize len = 0;
        const guint8* data = static_cast<const guint8*>(g_bytes_get_data(bytes, &len));
        if (!data || len == 0) {
            err = "Cover Art Archive response body is empty";
            g_clear_error(&gerr);
            g_bytes_unref(bytes);
            g_object_unref(msg);
            break;
        }
        body.assign(data, data + len);
        const gchar* ct = soup_message_headers_get_one(
            soup_message_get_response_headers(msg),
            "Content-Type");
        if (ct) content_type = ct;

        g_clear_error(&gerr);
        g_bytes_unref(bytes);
        g_object_unref(msg);
        ok = true;
        break;
    }

    g_object_unref(session);
    return ok;
}

}  // namespace

extern "C" {

int cdrip_fetch_cover_art(
    CdRipCddbEntry* entry,
    const CdRipDiscToc* toc,
    const char** error) {

    clear_error(error);
    if (!entry) {
        set_error(error, "Invalid entry for cover art fetch");
        return 0;
    }
    if (has_cover_art_data(entry->cover_art)) {
        return 1;
    }

    const std::string source_label = to_lower(to_string_or_empty(entry->source_label));
    if (source_label != "musicbrainz") {
        return 0;
    }
    // Respect MusicBrainz metadata: if it indicates no artwork, don't attempt downloading.
    if (entry->cover_art.available == 0) {
        return 0;
    }

    std::string release_id = album_tag(entry, "MUSICBRAINZ_RELEASE");
    if (release_id.empty() && toc) {
        release_id = to_string_or_empty(toc->mb_release_id);
    }
    const std::string release_group_id = album_tag(entry, "MUSICBRAINZ_RELEASEGROUPID");

    if (release_id.empty() && release_group_id.empty()) {
        return 0;
    }

    std::string content_type;
    std::vector<uint8_t> data;
    std::string err_msg;

    auto try_fetch = [&](const std::string& url) -> bool {
        std::vector<uint8_t> body;
        std::string ct;
        std::string local_err;
        if (!http_get_bytes(url, body, ct, local_err)) {
            if (!local_err.empty()) err_msg = local_err;
            return false;
        }
        data.swap(body);
        content_type = ct;
        return true;
    };

    bool success = false;
    if (!release_id.empty()) {
        const std::string url = "https://coverartarchive.org/release/" + release_id + "/front";
        success = try_fetch(url);
    }
    if (!success && !release_group_id.empty()) {
        const std::string url = "https://coverartarchive.org/release-group/" + release_group_id + "/front";
        success = try_fetch(url);
    }

    if (!success) {
        if (!err_msg.empty()) set_error(error, err_msg);
        return 0;
    }

    if (content_type.empty()) content_type = "image/jpeg";
    entry->cover_art.size = data.size();
    entry->cover_art.data = new uint8_t[entry->cover_art.size];
    std::copy(data.begin(), data.end(), const_cast<uint8_t*>(entry->cover_art.data));
    entry->cover_art.mime_type = make_cstr_copy(content_type);
    entry->cover_art.is_front = 1;
    entry->cover_art.available = 1;
    return 1;
}

};
