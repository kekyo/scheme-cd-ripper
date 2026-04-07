// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "internal.h"
#include "format_value.h"

using namespace cdrip::detail;

namespace {

const std::string reserved = "\\:?\"<>|*";

std::string sanitize_component(
    const std::string& input) {

    std::string result;
    result.reserve(input.size());
    for (unsigned char uch : input) {
        char ch = static_cast<char>(uch);
        if (std::iscntrl(uch) || reserved.find(ch) != std::string::npos || ch == '/') {
            result.push_back('_');
        } else {
            result.push_back(ch);
        }
    }
    if (result.empty()) result = "track";
    return result;
}

std::string sanitize_path_components(
    const std::string& path,
    bool leading_slash) {

    std::ostringstream oss;
    if (leading_slash) {
        oss << "/";
    }

    std::string part;
    std::istringstream iss(path);
    bool first = true;
    while (std::getline(iss, part, '/')) {
        if (!first) {
            oss << "/";
        }
        oss << sanitize_component(part);
        first = false;
    }
    return oss.str();
}

std::string sanitize_path(
    const std::string& path) {

    const auto scheme_pos = path.find("://");
    if (scheme_pos != std::string::npos) {
        const std::string scheme = path.substr(0, scheme_pos);
        const std::string rest = path.substr(scheme_pos + 3);
        const auto authority_end = rest.find('/');
        if (authority_end == std::string::npos) {
            return scheme + "://" + rest;
        }
        const std::string authority = rest.substr(0, authority_end);
        const std::string uri_path = rest.substr(authority_end + 1);
        return scheme + "://" + authority +
            sanitize_path_components(uri_path, /*leading_slash=*/true);
    }

    const bool leading_slash = !path.empty() && path.front() == '/';
    const std::string path_no_leading = leading_slash ? path.substr(1) : path;
    return sanitize_path_components(path_no_leading, leading_slash);
}

std::string truncate_on_newline(
    const std::string& s) {

    const size_t pos_ctrl = s.find_first_of("\r\n");
    const size_t pos_lit_n = s.find("\\n");
    const size_t pos_lit_r = s.find("\\r");

    size_t pos = std::string::npos;
    if (pos_ctrl != std::string::npos) pos = pos_ctrl;
    if (pos_lit_n != std::string::npos) pos = (pos == std::string::npos) ? pos_lit_n : std::min(pos, pos_lit_n);
    if (pos_lit_r != std::string::npos) pos = (pos == std::string::npos) ? pos_lit_r : std::min(pos, pos_lit_r);
    if (pos == std::string::npos) return s;
    return s.substr(0, pos);
}

bool is_numeric_format_key(
    const std::string& key_upper) {

    return key_upper == "TRACKNUMBER"
        || key_upper == "TRACKTOTAL"
        || key_upper == "DISCNUMBER"
        || key_upper == "DISCTOTAL"
        || key_upper == "CDDB_TOTAL_SECONDS"
        || key_upper == "MUSICBRAINZ_LEADOUT";
}

bool parse_int_strict(
    const std::string& s,
    int& out) {

    const std::string trimmed = trim(s);
    if (trimmed.empty()) return false;
    size_t idx = 0;
    try {
        int value = std::stoi(trimmed, &idx);
        if (idx != trimmed.size()) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

FormatTagMap build_format_tags(
    const std::map<std::string, std::string>& path_tags) {

    FormatTagMap format_tags;
    for (const auto& [key, value] : path_tags) {
        const std::string key_upper = to_upper(key);
        if (is_numeric_format_key(key_upper)) {
            int numeric = 0;
            if (parse_int_strict(value, numeric)) {
                format_tags[key_upper] = std::make_unique<NumericValue>(numeric, value);
                continue;
            }
        }
        format_tags[key_upper] = std::make_unique<StringValue>(value);
    }
    return format_tags;
}

std::string format_filename(
    const std::string& fmt,
    const FormatTagMap& tags) {

    std::string out;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '{') {
            size_t end = fmt.find('}', i);
            if (end != std::string::npos) {
                std::string token = fmt.substr(i + 1, end - i - 1);
                const FormatExpression expr = parse_format_expression(token);
                out += format_token_expression(expr, tags);
                i = end;
                continue;
            }
        }
        out.push_back(fmt[i]);
    }
    if (out.size() < 5 || out.substr(out.size() - 5) != ".flac") {
        out += ".flac";
    }
    return sanitize_path(out);
}

}

namespace cdrip::detail {

std::map<std::string, std::string> build_track_vorbis_tags(
    const CdRipTrackInfo* track,
    const CdRipCddbEntry* meta,
    const CdRipDiscToc* toc,
    int total_tracks,
    std::string& title_out,
    std::string& track_name_out,
    std::string& safe_title_out) {

    title_out.clear();
    track_name_out.clear();
    safe_title_out.clear();
    if (!track || !meta || !toc) return {};

    const std::string meta_title = track_tag(meta, static_cast<size_t>(track->number - 1), "TITLE");
    const std::string title = !meta_title.empty()
        ? meta_title
        : ("Track " + std::to_string(track->number));
    const std::string track_name = truncate_on_newline(title);
    const std::string safe_title = format_safe_string(track_name);
    const std::string meta_artist = album_tag(meta, "ARTIST");
    const std::string meta_album = album_tag(meta, "ALBUM");
    const std::string meta_genre = album_tag(meta, "GENRE");
    const std::string meta_year = album_tag(meta, "DATE");
    const std::string meta_discid = to_string_or_empty(meta->cddb_discid);
    const std::string meta_source_label = to_string_or_empty(meta->source_label);
    const std::string meta_source_url = to_string_or_empty(meta->source_url);
    const std::string meta_fetched_at = to_string_or_empty(meta->fetched_at);
    const bool ignore_source = meta_source_label.empty() && meta_source_url.empty();

    std::string fetched_for_tag;
    if (!ignore_source && meta_fetched_at.empty()) {
        char* ts = cdrip_current_timestamp_iso();
        fetched_for_tag = to_string_or_empty(ts);
        cdrip_release_timestamp(ts);
    } else {
        fetched_for_tag = meta_fetched_at;
    }

    const std::string cddb_discid = !meta_discid.empty()
        ? meta_discid
        : to_string_or_empty(toc->cddb_discid);

    std::map<std::string, std::string> tags = {
        {"TITLE", title},
        {"ARTIST", meta_artist},
        {"ALBUM", meta_album},
        {"GENRE", meta_genre},
        {"DATE", meta_year},
    };
    append_requery_seed_tags(tags, toc, cddb_discid, track->number, total_tracks);
    if (!ignore_source) {
        tags["CDDB"] = meta_source_label;
        tags["CDDB_DATE"] = fetched_for_tag;
    }

    if (meta->album_tags && meta->album_tags_count > 0) {
        apply_tag_kvs(tags, meta->album_tags, meta->album_tags_count);
    }
    if (meta->tracks && static_cast<size_t>(track->number - 1) < meta->tracks_count) {
        const auto& tt = meta->tracks[static_cast<size_t>(track->number - 1)];
        if (tt.tags && tt.tags_count > 0) {
            apply_tag_kvs(tags, tt.tags, tt.tags_count);
        }
    }

    prune_empty_tags(tags);
    title_out = title;
    track_name_out = track_name;
    safe_title_out = safe_title;
    return tags;
}

bool resolve_track_output_path(
    const std::string& format,
    const std::map<std::string, std::string>& tags,
    std::string& out_path,
    std::string& err) {

    err.clear();
    std::map<std::string, std::string> path_tags = tags;
    for (auto& [key, value] : path_tags) {
        value = truncate_on_newline(value);
    }

    try {
        FormatTagMap format_tags = build_format_tags(path_tags);
        out_path = format_filename(format, format_tags);
        return true;
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    } catch (...) {
        err = "Failed to resolve output path";
        return false;
    }
}

}
