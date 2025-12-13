#pragma once

// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gio/gio.h>
#include <libsoup/soup.h>

namespace cdrip::detail {

struct HttpRetryPolicy {
    int timeout_sec = 10;
    int max_attempts = 3;
    int retry_delay_ms = 1200;
    int max_redirects = 2;
    bool respect_retry_after = true;
};

static inline bool http_status_is_retryable(guint status) {
    if (status == 0) return true;  // network error / not reached server
    if (status == 408) return true;  // Request Timeout
    if (status == 429) return true;  // Too Many Requests
    if (status >= 500 && status <= 599) return true;  // transient server errors
    return false;
}

static inline bool gerror_is_retryable(const GError* gerr) {
    if (!gerr) return false;

    // Transient I/O errors that may occur due to flaky networks or server side disconnects.
    if (g_error_matches(gerr, G_IO_ERROR, G_IO_ERROR_FAILED) ||
        g_error_matches(gerr, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED) ||
        g_error_matches(gerr, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
        g_error_matches(gerr, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED) ||
        g_error_matches(gerr, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE) ||
        g_error_matches(gerr, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE) ||
        g_error_matches(gerr, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED) ||
        g_error_matches(gerr, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE)) {
        return true;
    }

    // TLS handshake failures can be transient (server resets, middleboxes, etc.).
    // Do not retry hard failures like bad certificates.
    if (g_error_matches(gerr, G_TLS_ERROR, G_TLS_ERROR_HANDSHAKE) ||
        g_error_matches(gerr, G_TLS_ERROR, G_TLS_ERROR_MISC) ||
        g_error_matches(gerr, G_TLS_ERROR, G_TLS_ERROR_UNAVAILABLE)) {
        return true;
    }

    return false;
}

static inline int parse_retry_after_ms(const char* value) {
    if (!value) return -1;
    char* end = nullptr;
    long sec = std::strtol(value, &end, 10);
    if (end == value) return -1;
    if (sec <= 0) return -1;
    if (sec > 60 * 60) sec = 60 * 60;
    const long ms = sec * 1000L;
    if (ms > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    return static_cast<int>(ms);
}

static inline int compute_retry_delay_ms(
    const HttpRetryPolicy& policy,
    SoupMessage* msg) {

    if (policy.respect_retry_after && msg) {
        const char* retry_after = soup_message_headers_get_one(
            soup_message_get_response_headers(msg),
            "Retry-After");
        const int ra_ms = parse_retry_after_ms(retry_after);
        if (ra_ms > 0) return ra_ms;
    }
    return std::max(0, policy.retry_delay_ms);
}

static inline bool http_get_bytes_with_retry(
    const std::string& service_name,
    const std::string& url,
    const std::string& user_agent,
    const char* accept,
    const HttpRetryPolicy& policy,
    std::vector<uint8_t>& body,
    std::string& content_type,
    std::string& err) {

    body.clear();
    content_type.clear();
    err.clear();

    SoupSession* session = soup_session_new();
    if (!session) {
        err = "Failed to create SoupSession";
        return false;
    }
    g_object_set(
        session,
        "user-agent",
        user_agent.c_str(),
        "timeout",
        std::max(1, policy.timeout_sec),
        nullptr);

    bool ok = false;
    std::string current_url = url;
    int redirects = 0;

    for (int attempt = 0; attempt < std::max(1, policy.max_attempts); ++attempt) {
        SoupMessage* msg = soup_message_new("GET", current_url.c_str());
        if (!msg) {
            err = "Failed to create SoupMessage";
            break;
        }
        if (accept && accept[0] != '\0') {
            soup_message_headers_replace(
                soup_message_get_request_headers(msg),
                "Accept",
                accept);
        }

        GError* gerr = nullptr;
        GBytes* bytes = soup_session_send_and_read(session, msg, nullptr, &gerr);
        const guint status = soup_message_get_status(msg);

        if (SOUP_STATUS_IS_REDIRECTION(status)) {
            const char* loc = soup_message_headers_get_one(
                soup_message_get_response_headers(msg),
                "Location");
            if (loc && redirects < std::max(0, policy.max_redirects)) {
                current_url = loc;
                ++redirects;
                g_clear_error(&gerr);
                g_object_unref(msg);
                if (bytes) g_bytes_unref(bytes);
                --attempt;  // redirects do not consume attempts
                continue;
            }
        }

        const gchar* ct = soup_message_headers_get_one(
            soup_message_get_response_headers(msg),
            "Content-Type");
        if (ct) content_type = ct;

        if (SOUP_STATUS_IS_SUCCESSFUL(status) && bytes) {
            gsize len = 0;
            const guint8* data = static_cast<const guint8*>(g_bytes_get_data(bytes, &len));
            if (data && len > 0) {
                body.assign(data, data + len);
                g_clear_error(&gerr);
                g_bytes_unref(bytes);
                g_object_unref(msg);
                ok = true;
                break;
            }
        }

        const bool retry_allowed = (attempt + 1) < std::max(1, policy.max_attempts);

        const bool empty_success_body =
            SOUP_STATUS_IS_SUCCESSFUL(status) &&
            (!bytes || g_bytes_get_size(bytes) == 0);

        const bool should_retry =
            retry_allowed &&
            (http_status_is_retryable(status) || gerror_is_retryable(gerr) || empty_success_body);

        if (should_retry) {
            const int delay_ms = compute_retry_delay_ms(policy, msg);
            g_clear_error(&gerr);
            g_object_unref(msg);
            if (bytes) g_bytes_unref(bytes);
            if (delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
            continue;
        }

        if (SOUP_STATUS_IS_SUCCESSFUL(status)) {
            err = service_name + " response body is empty";
            g_clear_error(&gerr);
            g_object_unref(msg);
            if (bytes) g_bytes_unref(bytes);
            break;
        }

        std::string resp_body;
        if (bytes) {
            gsize elen = 0;
            const gchar* edata = static_cast<const gchar*>(g_bytes_get_data(bytes, &elen));
            if (edata && elen > 0) resp_body.assign(edata, elen);
        }
        std::ostringstream oss;
        oss << service_name << " request failed with status " << status;
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

    g_object_unref(session);
    return ok;
}

}  // namespace cdrip::detail
