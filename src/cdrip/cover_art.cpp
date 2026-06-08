// Scheme CD music/sound ripper
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/scheme-cd-ripper

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <jpeglib.h>
#include <lcms2.h>
#include <png.h>

#include <glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "internal.h"
#include "http_retry.h"
#include "version.h"

using namespace cdrip::detail;

namespace {

constexpr int kCoverArtTimeoutSec = 15;
constexpr int kCoverArtRetryDelayMs = 1200;

constexpr int kDefaultCoverArtMaxWidth = 512;
constexpr size_t kMaxFlacPictureBytes = 16 * 1024 * 1024 - 1;

std::atomic<int> g_cover_art_max_width{kDefaultCoverArtMaxWidth};

static std::string cover_art_user_agent() {
    std::string ua = "SchemeCDRipper/";
    ua += VERSION;
    ua += " (https://github.com/kekyo/scheme-cd-ripper)";
    return ua;
}

enum class PixelLayout {
    kGray8,
    kRGB8,
    kRGBA8,
    kCMYK8,
};

struct ImageBuffer {
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::kRGB8;
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> icc_profile;
    bool cmyk_inverted = false;
};

static bool is_png_data(const std::vector<uint8_t>& data) {
    static constexpr uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return data.size() >= 8 && std::memcmp(data.data(), kPngSig, 8) == 0;
}

static bool is_jpeg_data(const std::vector<uint8_t>& data) {
    return data.size() >= 2 && data[0] == 0xFF && data[1] == 0xD8;
}

struct PngReadContext {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t offset = 0;
};

static void png_read_callback(png_structp png_ptr, png_bytep out_bytes, png_size_t byte_count) {
    auto* ctx = static_cast<PngReadContext*>(png_get_io_ptr(png_ptr));
    if (!ctx || !ctx->data || ctx->offset + byte_count > ctx->size) {
        png_error(png_ptr, "Invalid PNG read");
        return;
    }
    std::memcpy(out_bytes, ctx->data + ctx->offset, byte_count);
    ctx->offset += byte_count;
}

static bool decode_png_to_rgba(
    const std::vector<uint8_t>& input,
    ImageBuffer& out,
    std::string& err) {

    if (!is_png_data(input)) {
        err = "Not a PNG image";
        return false;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) {
        err = "Failed to create PNG read struct";
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        err = "Failed to create PNG info struct";
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        err = "Failed to decode PNG";
        return false;
    }

    PngReadContext ctx{input.data(), input.size(), 0};
    png_set_read_fn(png_ptr, &ctx, png_read_callback);

    png_read_info(png_ptr, info_ptr);

    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace = 0;
    int compression = 0;
    int filter = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, &interlace, &compression, &filter);

    // Extract ICC profile if present. Prefer iCCP over sRGB chunk.
    png_charp profile_name = nullptr;
    int compression_type = 0;
    png_bytep profile_data = nullptr;
    png_uint_32 profile_len = 0;
    if (png_get_iCCP(png_ptr, info_ptr, &profile_name, &compression_type, &profile_data, &profile_len) == PNG_INFO_iCCP) {
        if (profile_data && profile_len > 0) {
            out.icc_profile.assign(profile_data, profile_data + profile_len);
        }
    } else {
        int intent = 0;
        if (png_get_sRGB(png_ptr, info_ptr, &intent) == PNG_INFO_sRGB) {
            out.icc_profile.clear();  // already sRGB
        }
    }

    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    const png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    const int channels = png_get_channels(png_ptr, info_ptr);
    if (channels != 3 && channels != 4) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        err = "Unsupported PNG channel count";
        return false;
    }

    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.layout = (channels == 4) ? PixelLayout::kRGBA8 : PixelLayout::kRGB8;
    out.pixels.resize(rowbytes * height);

    std::vector<png_bytep> rows(height);
    for (png_uint_32 y = 0; y < height; ++y) {
        rows[y] = reinterpret_cast<png_bytep>(out.pixels.data() + y * rowbytes);
    }
    png_read_image(png_ptr, rows.data());
    png_read_end(png_ptr, nullptr);
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);

    // Normalize to RGBA8 for downstream handling.
    if (channels == 3) {
        std::vector<uint8_t> rgba(static_cast<size_t>(out.width) * out.height * 4);
        for (int y = 0; y < out.height; ++y) {
            const uint8_t* src = out.pixels.data() + static_cast<size_t>(y) * rowbytes;
            uint8_t* dst = rgba.data() + static_cast<size_t>(y) * out.width * 4;
            for (int x = 0; x < out.width; ++x) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = 255;
                src += 3;
                dst += 4;
            }
        }
        out.pixels.swap(rgba);
        out.layout = PixelLayout::kRGBA8;
    } else {
        // libpng may include padding in rowbytes; repack to tightly packed RGBA.
        if (rowbytes != static_cast<png_size_t>(out.width) * 4) {
            std::vector<uint8_t> rgba(static_cast<size_t>(out.width) * out.height * 4);
            for (int y = 0; y < out.height; ++y) {
                const uint8_t* src = out.pixels.data() + static_cast<size_t>(y) * rowbytes;
                uint8_t* dst = rgba.data() + static_cast<size_t>(y) * out.width * 4;
                std::memcpy(dst, src, static_cast<size_t>(out.width) * 4);
            }
            out.pixels.swap(rgba);
        }
        out.layout = PixelLayout::kRGBA8;
    }

    return true;
}

struct JpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
    char message[JMSG_LENGTH_MAX]{};
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->setjmp_buffer, 1);
}

static bool jpeg_has_adobe_marker(const jpeg_decompress_struct& cinfo) {
    for (jpeg_saved_marker_ptr m = cinfo.marker_list; m != nullptr; m = m->next) {
        if (m->marker != (JPEG_APP0 + 14)) continue;
        if (m->data_length < 12) continue;
        if (std::memcmp(m->data, "Adobe", 5) == 0) return true;
    }
    return false;
}

static bool extract_jpeg_icc_profile(
    const jpeg_decompress_struct& cinfo,
    std::vector<uint8_t>& out_profile) {

    struct Segment {
        int seq = 0;
        int count = 0;
        std::vector<uint8_t> data;
    };
    std::vector<Segment> segments;
    int expected_count = 0;

    for (jpeg_saved_marker_ptr m = cinfo.marker_list; m != nullptr; m = m->next) {
        if (m->marker != (JPEG_APP0 + 2)) continue;
        if (m->data_length < 14) continue;
        if (std::memcmp(m->data, "ICC_PROFILE\0", 12) != 0) continue;

        const int seq = m->data[12];
        const int count = m->data[13];
        if (seq <= 0 || count <= 0) continue;
        if (expected_count == 0) expected_count = count;
        if (count != expected_count) continue;

        const size_t payload_len = m->data_length - 14;
        Segment s{};
        s.seq = seq;
        s.count = count;
        s.data.assign(m->data + 14, m->data + 14 + payload_len);
        segments.push_back(std::move(s));
    }

    if (expected_count <= 0 || segments.empty()) return false;

    std::vector<std::vector<uint8_t>> ordered(static_cast<size_t>(expected_count));
    for (const auto& s : segments) {
        if (s.seq <= 0 || s.seq > expected_count) continue;
        ordered[static_cast<size_t>(s.seq - 1)] = s.data;
    }
    for (int i = 0; i < expected_count; ++i) {
        if (ordered[static_cast<size_t>(i)].empty()) return false;
    }
    size_t total = 0;
    for (const auto& part : ordered) total += part.size();
    out_profile.clear();
    out_profile.reserve(total);
    for (const auto& part : ordered) out_profile.insert(out_profile.end(), part.begin(), part.end());
    return !out_profile.empty();
}

static bool decode_jpeg(
    const std::vector<uint8_t>& input,
    ImageBuffer& out,
    std::string& err) {

    if (!is_jpeg_data(input)) {
        err = "Not a JPEG image";
        return false;
    }

    jpeg_decompress_struct cinfo{};
    JpegErrorMgr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        err = jerr.message[0] != '\0' ? jerr.message : "Failed to decode JPEG";
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<unsigned char*>(input.data()), input.size());

    // ICC profile is split across APP2 markers.
    jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);
    // Adobe marker sometimes indicates inverted CMYK.
    jpeg_save_markers(&cinfo, JPEG_APP0 + 14, 0xFFFF);

    jpeg_read_header(&cinfo, TRUE);

    extract_jpeg_icc_profile(cinfo, out.icc_profile);
    out.cmyk_inverted = jpeg_has_adobe_marker(cinfo);

    const J_COLOR_SPACE cs = cinfo.jpeg_color_space;
    if (cs == JCS_GRAYSCALE) {
        cinfo.out_color_space = JCS_GRAYSCALE;
        out.layout = PixelLayout::kGray8;
    } else if (cs == JCS_CMYK || cs == JCS_YCCK) {
        cinfo.out_color_space = JCS_CMYK;
        out.layout = PixelLayout::kCMYK8;
    } else {
        cinfo.out_color_space = JCS_RGB;
        out.layout = PixelLayout::kRGB8;
    }

    jpeg_start_decompress(&cinfo);

    out.width = static_cast<int>(cinfo.output_width);
    out.height = static_cast<int>(cinfo.output_height);
    const int components = static_cast<int>(cinfo.output_components);
    const size_t row_stride = static_cast<size_t>(out.width) * components;
    out.pixels.resize(static_cast<size_t>(out.height) * row_stride);

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rowptr[1];
        rowptr[0] = reinterpret_cast<JSAMPROW>(
            out.pixels.data() + static_cast<size_t>(cinfo.output_scanline) * row_stride);
        jpeg_read_scanlines(&cinfo, rowptr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    if ((out.layout == PixelLayout::kGray8 && components != 1) ||
        (out.layout == PixelLayout::kRGB8 && components != 3) ||
        (out.layout == PixelLayout::kCMYK8 && components != 4)) {
        err = "Unexpected JPEG decoded component count";
        return false;
    }

    return true;
}

static bool convert_cmyk_to_srgb_approx(
    const ImageBuffer& input,
    std::vector<uint8_t>& out_rgb) {

    if (input.layout != PixelLayout::kCMYK8) return false;
    if (input.width <= 0 || input.height <= 0) return false;
    const size_t pixels_count = static_cast<size_t>(input.width) * input.height;
    if (input.pixels.size() != pixels_count * 4) return false;
    out_rgb.resize(pixels_count * 3);

    const uint8_t* src = input.pixels.data();
    uint8_t* dst = out_rgb.data();
    for (size_t i = 0; i < pixels_count; ++i) {
        int c = src[0];
        int m = src[1];
        int y = src[2];
        int k = src[3];
        if (input.cmyk_inverted) {
            c = 255 - c;
            m = 255 - m;
            y = 255 - y;
            k = 255 - k;
        }
        const int r = (255 - c) * (255 - k) / 255;
        const int g = (255 - m) * (255 - k) / 255;
        const int b = (255 - y) * (255 - k) / 255;
        dst[0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
        dst[1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
        dst[2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
        src += 4;
        dst += 3;
    }
    return true;
}

static bool apply_icc_transform_to_srgb(
    const ImageBuffer& input,
    std::vector<uint8_t>& out_rgb,
    std::string& err) {

    if (input.icc_profile.empty()) {
        err = "ICC profile not available";
        return false;
    }

    cmsHPROFILE in_prof = cmsOpenProfileFromMem(input.icc_profile.data(), input.icc_profile.size());
    if (!in_prof) {
        err = "Failed to open ICC profile";
        return false;
    }
    cmsHPROFILE out_prof = cmsCreate_sRGBProfile();
    if (!out_prof) {
        cmsCloseProfile(in_prof);
        err = "Failed to create sRGB profile";
        return false;
    }

    cmsUInt32Number in_fmt = 0;
    std::vector<uint8_t> in_pixels;
    const size_t pixels_count = static_cast<size_t>(input.width) * input.height;

    const cmsColorSpaceSignature cs = cmsGetColorSpace(in_prof);
    if (input.layout == PixelLayout::kCMYK8) {
        in_fmt = TYPE_CMYK_8;
        if (input.cmyk_inverted) {
            in_pixels = input.pixels;
            for (auto& b : in_pixels) b = static_cast<uint8_t>(255 - b);
        }
    } else if (input.layout == PixelLayout::kGray8 || cs == cmsSigGrayData) {
        // If the profile is Gray, feed single-channel grayscale (derive from R when needed).
        in_fmt = TYPE_GRAY_8;
        in_pixels.resize(pixels_count);
        if (input.layout == PixelLayout::kGray8) {
            in_pixels = input.pixels;
        } else if (input.layout == PixelLayout::kRGB8) {
            for (size_t i = 0; i < pixels_count; ++i) {
                in_pixels[i] = input.pixels[i * 3];
            }
        } else if (input.layout == PixelLayout::kRGBA8) {
            for (size_t i = 0; i < pixels_count; ++i) {
                in_pixels[i] = input.pixels[i * 4];
            }
        } else {
            cmsCloseProfile(out_prof);
            cmsCloseProfile(in_prof);
            err = "Unsupported input layout for Gray ICC profile";
            return false;
        }
    } else {
        in_fmt = TYPE_RGB_8;
        if (input.layout == PixelLayout::kRGB8) {
            // use directly
        } else if (input.layout == PixelLayout::kRGBA8) {
            in_pixels.resize(pixels_count * 3);
            for (size_t i = 0; i < pixels_count; ++i) {
                in_pixels[i * 3 + 0] = input.pixels[i * 4 + 0];
                in_pixels[i * 3 + 1] = input.pixels[i * 4 + 1];
                in_pixels[i * 3 + 2] = input.pixels[i * 4 + 2];
            }
        } else if (input.layout == PixelLayout::kGray8) {
            in_pixels.resize(pixels_count * 3);
            for (size_t i = 0; i < pixels_count; ++i) {
                const uint8_t g = input.pixels[i];
                in_pixels[i * 3 + 0] = g;
                in_pixels[i * 3 + 1] = g;
                in_pixels[i * 3 + 2] = g;
            }
        } else {
            cmsCloseProfile(out_prof);
            cmsCloseProfile(in_prof);
            err = "Unsupported input layout for ICC transform";
            return false;
        }
    }

    cmsUInt32Number out_fmt = TYPE_RGB_8;
    cmsHTRANSFORM xform = cmsCreateTransform(
        in_prof, in_fmt, out_prof, out_fmt, INTENT_PERCEPTUAL, cmsFLAGS_COPY_ALPHA);
    if (!xform) {
        cmsCloseProfile(out_prof);
        cmsCloseProfile(in_prof);
        err = "Failed to create ICC transform";
        return false;
    }

    out_rgb.resize(pixels_count * 3);
    const void* src_ptr = nullptr;
    if (in_pixels.empty()) {
        src_ptr = input.pixels.data();
    } else {
        src_ptr = in_pixels.data();
    }
    cmsDoTransform(xform, src_ptr, out_rgb.data(), pixels_count);

    cmsDeleteTransform(xform);
    cmsCloseProfile(out_prof);
    cmsCloseProfile(in_prof);
    return true;
}

static void expand_gray_to_rgb(
    const ImageBuffer& input,
    std::vector<uint8_t>& out_rgb) {

    const size_t pixels_count = static_cast<size_t>(input.width) * input.height;
    out_rgb.resize(pixels_count * 3);
    for (size_t i = 0; i < pixels_count; ++i) {
        const uint8_t g = input.pixels[i];
        out_rgb[i * 3 + 0] = g;
        out_rgb[i * 3 + 1] = g;
        out_rgb[i * 3 + 2] = g;
    }
}

static bool convert_to_srgb(
    ImageBuffer& img,
    std::string& err) {

    if (img.width <= 0 || img.height <= 0) {
        err = "Invalid image dimensions";
        return false;
    }

    // Apply ICC profile when available.
    if (!img.icc_profile.empty()) {
        std::vector<uint8_t> out_rgb;
        std::string xerr;
        if (!apply_icc_transform_to_srgb(img, out_rgb, xerr)) {
            err = xerr;
            return false;
        }

        if (img.layout == PixelLayout::kRGBA8) {
            const size_t pixels_count = static_cast<size_t>(img.width) * img.height;
            std::vector<uint8_t> out_rgba(pixels_count * 4);
            for (size_t i = 0; i < pixels_count; ++i) {
                out_rgba[i * 4 + 0] = out_rgb[i * 3 + 0];
                out_rgba[i * 4 + 1] = out_rgb[i * 3 + 1];
                out_rgba[i * 4 + 2] = out_rgb[i * 3 + 2];
                out_rgba[i * 4 + 3] = img.pixels[i * 4 + 3];
            }
            img.pixels.swap(out_rgba);
            img.layout = PixelLayout::kRGBA8;
        } else {
            img.pixels.swap(out_rgb);
            img.layout = PixelLayout::kRGB8;
        }
        img.icc_profile.clear();
        return true;
    }

    // No ICC: treat as sRGB already.
    if (img.layout == PixelLayout::kGray8) {
        std::vector<uint8_t> rgb;
        expand_gray_to_rgb(img, rgb);
        img.pixels.swap(rgb);
        img.layout = PixelLayout::kRGB8;
        return true;
    }
    if (img.layout == PixelLayout::kCMYK8) {
        std::vector<uint8_t> rgb;
        if (!convert_cmyk_to_srgb_approx(img, rgb)) {
            err = "Failed to convert CMYK image to sRGB";
            return false;
        }
        img.pixels.swap(rgb);
        img.layout = PixelLayout::kRGB8;
        return true;
    }
    // RGB/RGBA: no-op
    return true;
}

static void resize_bilinear(
    const uint8_t* src,
    int src_w,
    int src_h,
    int channels,
    uint8_t* dst,
    int dst_w,
    int dst_h) {

    const float x_scale = static_cast<float>(src_w) / dst_w;
    const float y_scale = static_cast<float>(src_h) / dst_h;

    for (int y = 0; y < dst_h; ++y) {
        const float sy = (y + 0.5f) * y_scale - 0.5f;
        int y0 = static_cast<int>(std::floor(sy));
        int y1 = y0 + 1;
        const float wy = sy - y0;
        if (y0 < 0) { y0 = 0; }
        if (y1 >= src_h) { y1 = src_h - 1; }
        for (int x = 0; x < dst_w; ++x) {
            const float sx = (x + 0.5f) * x_scale - 0.5f;
            int x0 = static_cast<int>(std::floor(sx));
            int x1 = x0 + 1;
            const float wx = sx - x0;
            if (x0 < 0) { x0 = 0; }
            if (x1 >= src_w) { x1 = src_w - 1; }

            const uint8_t* p00 = src + (static_cast<size_t>(y0) * src_w + x0) * channels;
            const uint8_t* p10 = src + (static_cast<size_t>(y0) * src_w + x1) * channels;
            const uint8_t* p01 = src + (static_cast<size_t>(y1) * src_w + x0) * channels;
            const uint8_t* p11 = src + (static_cast<size_t>(y1) * src_w + x1) * channels;

            uint8_t* out = dst + (static_cast<size_t>(y) * dst_w + x) * channels;
            for (int c = 0; c < channels; ++c) {
                const float v00 = p00[c];
                const float v10 = p10[c];
                const float v01 = p01[c];
                const float v11 = p11[c];
                const float v0 = v00 + (v10 - v00) * wx;
                const float v1 = v01 + (v11 - v01) * wx;
                const float v = v0 + (v1 - v0) * wy;
                out[c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
            }
        }
    }
}

struct PngWriteContext {
    std::vector<uint8_t>* out = nullptr;
};

static void png_write_callback(png_structp png_ptr, png_bytep data, png_size_t length) {
    auto* ctx = static_cast<PngWriteContext*>(png_get_io_ptr(png_ptr));
    if (!ctx || !ctx->out) {
        png_error(png_ptr, "Invalid PNG write");
        return;
    }
    ctx->out->insert(ctx->out->end(), data, data + length);
}

static void png_flush_callback(png_structp) {
}

static bool encode_png_from_pixels(
    const uint8_t* pixels,
    int width,
    int height,
    int channels,
    std::vector<uint8_t>& out_bytes,
    std::string& err) {

    if (!pixels || width <= 0 || height <= 0) {
        err = "Invalid image for PNG encode";
        return false;
    }
    if (channels != 3 && channels != 4) {
        err = "Unsupported channel count for PNG encode";
        return false;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) {
        err = "Failed to create PNG write struct";
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        err = "Failed to create PNG info struct";
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        err = "Failed to encode PNG";
        return false;
    }

    out_bytes.clear();
    out_bytes.reserve(static_cast<size_t>(width) * height);
    PngWriteContext ctx{&out_bytes};
    png_set_write_fn(png_ptr, &ctx, png_write_callback, png_flush_callback);

    const int color_type = (channels == 4) ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB;
    png_set_IHDR(
        png_ptr,
        info_ptr,
        width,
        height,
        8,
        color_type,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_BASE,
        PNG_FILTER_TYPE_BASE);

    // Indicate sRGB; omit embedded ICC to maximize compatibility.
    png_set_sRGB_gAMA_and_cHRM(png_ptr, info_ptr, PNG_sRGB_INTENT_PERCEPTUAL);

    png_write_info(png_ptr, info_ptr);

    const size_t rowbytes = static_cast<size_t>(width) * channels;
    std::vector<png_bytep> rows(static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        rows[static_cast<size_t>(y)] = reinterpret_cast<png_bytep>(
            const_cast<uint8_t*>(pixels + static_cast<size_t>(y) * rowbytes));
    }
    png_write_image(png_ptr, rows.data());
    png_write_end(png_ptr, info_ptr);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    return true;
}

static bool normalize_image_to_png(
    const std::vector<uint8_t>& input,
    int max_width_px,
    std::vector<uint8_t>& out_png,
    std::string& err) {

    ImageBuffer decoded;
    std::string derr;
    if (is_png_data(input)) {
        if (!decode_png_to_rgba(input, decoded, derr)) {
            err = derr;
            return false;
        }
    } else if (is_jpeg_data(input)) {
        if (!decode_jpeg(input, decoded, derr)) {
            err = derr;
            return false;
        }
    } else {
        err = "Unsupported image format";
        return false;
    }

    std::string cerr;
    if (!convert_to_srgb(decoded, cerr)) {
        err = "Color conversion failed: " + cerr;
        return false;
    }

    int channels = 0;
    if (decoded.layout == PixelLayout::kRGB8) channels = 3;
    else if (decoded.layout == PixelLayout::kRGBA8) channels = 4;
    else {
        err = "Unexpected pixel layout after conversion";
        return false;
    }

    int effective_max_width = max_width_px;
    if (effective_max_width <= 0) effective_max_width = kDefaultCoverArtMaxWidth;
    effective_max_width = std::max(1, effective_max_width);
    effective_max_width = std::min(effective_max_width, decoded.width);

    while (true) {
        const int target_w = std::min(decoded.width, effective_max_width);
        int target_h = decoded.height;
        if (decoded.width > 0 && target_w != decoded.width) {
            const double scale = static_cast<double>(target_w) / decoded.width;
            target_h = std::max(1, static_cast<int>(std::lround(decoded.height * scale)));
        }

        std::vector<uint8_t> scaled;
        const uint8_t* src = decoded.pixels.data();
        int src_w = decoded.width;
        int src_h = decoded.height;

        if (target_w != src_w || target_h != src_h) {
            scaled.resize(static_cast<size_t>(target_w) * target_h * channels);
            resize_bilinear(src, src_w, src_h, channels, scaled.data(), target_w, target_h);
            src = scaled.data();
            src_w = target_w;
            src_h = target_h;
        }

        std::vector<uint8_t> png;
        std::string perr;
        if (!encode_png_from_pixels(src, src_w, src_h, channels, png, perr)) {
            err = perr;
            return false;
        }

        if (png.size() <= kMaxFlacPictureBytes) {
            out_png.swap(png);
            return true;
        }

        if (effective_max_width <= 1) {
            err = "PNG exceeds FLAC picture size limit";
            return false;
        }
        effective_max_width = std::max(1, effective_max_width / 2);
    }
}

static bool http_get_bytes(
    const std::string& url,
    std::vector<uint8_t>& body,
    std::string& content_type,
    std::string& err) {
    HttpRetryPolicy policy{};
    policy.timeout_sec = kCoverArtTimeoutSec;
    policy.max_attempts = 3;
    policy.retry_delay_ms = kCoverArtRetryDelayMs;
    policy.max_redirects = 2;
    policy.respect_retry_after = true;

    return http_get_bytes_with_retry(
        "Cover Art Archive",
        url,
        cover_art_user_agent(),
        "image/*",
        policy,
        body,
        content_type,
        err);
}

static bool http_get_discogs_json(
    const std::string& url,
    std::string& body,
    std::string& err) {

    HttpRetryPolicy policy{};
    policy.timeout_sec = kCoverArtTimeoutSec;
    policy.max_attempts = 3;
    policy.retry_delay_ms = kCoverArtRetryDelayMs;
    policy.max_redirects = 2;
    policy.respect_retry_after = true;

    std::vector<uint8_t> bytes;
    std::string ct;
    if (!http_get_bytes_with_retry(
            "Discogs",
            url,
            cover_art_user_agent(),
            "application/json",
            policy,
            bytes,
            ct,
            err)) {
        return false;
    }

    body.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

static std::string json_get_string_member(JsonObject* obj, const char* name) {
    if (!obj || !name) return {};
    if (!json_object_has_member(obj, name)) return {};
    JsonNode* node = json_object_get_member(obj, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return {};
    const GType type = json_node_get_value_type(node);
    if (type == G_TYPE_STRING) {
        const gchar* value = json_node_get_string(node);
        return value ? std::string{value} : std::string{};
    }
    if (type == G_TYPE_INT64 || type == G_TYPE_INT || type == G_TYPE_LONG ||
        type == G_TYPE_UINT64 || type == G_TYPE_UINT || type == G_TYPE_ULONG) {
        return std::to_string(static_cast<long long>(json_node_get_int(node)));
    }
    return {};
}

static JsonArray* json_get_array_member(JsonObject* obj, const char* name) {
    if (!obj || !name) return nullptr;
    if (!json_object_has_member(obj, name)) return nullptr;
    return json_object_get_array_member(obj, name);
}

static JsonObject* json_get_object_member(JsonObject* obj, const char* name) {
    if (!obj || !name) return nullptr;
    if (!json_object_has_member(obj, name)) return nullptr;
    return json_object_get_object_member(obj, name);
}

static int json_get_int_member(JsonObject* obj, const char* name, int fallback = 0) {
    if (!obj || !name || !json_object_has_member(obj, name)) return fallback;
    JsonNode* node = json_object_get_member(obj, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return fallback;
    try {
        return static_cast<int>(json_node_get_int(node));
    } catch (...) {
        return fallback;
    }
}

static std::string select_discogs_image_url(JsonObject* release_obj) {
    JsonArray* images = json_get_array_member(release_obj, "images");
    if (!images) return {};
    const guint len = json_array_get_length(images);
    std::string first_any;
    for (guint i = 0; i < len; ++i) {
        JsonObject* img = json_array_get_object_element(images, i);
        if (!img) continue;
        std::string uri = json_get_string_member(img, "uri");
        if (uri.empty()) uri = json_get_string_member(img, "resource_url");
        if (uri.empty()) continue;
        if (first_any.empty()) first_any = uri;
        const std::string type = to_lower(json_get_string_member(img, "type"));
        if (type == "primary") return uri;
    }
    return first_any;
}

static std::string strip_discogs_artist_suffix(const std::string& input) {
    std::string value = trim(input);
    while (!value.empty() && value.back() == '*') {
        value.pop_back();
        value = trim(value);
    }
    if (value.size() < 4 || value.back() != ')') return value;
    const size_t open = value.find_last_of('(');
    if (open == std::string::npos || open == 0) return value;
    const std::string suffix = value.substr(open + 1, value.size() - open - 2);
    if (suffix.empty()) return value;
    for (unsigned char ch : suffix) {
        if (!std::isdigit(ch)) return value;
    }
    return trim(value.substr(0, open));
}

static std::string normalize_discogs_compare_text(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool last_space = false;
    for (unsigned char ch : input) {
        if (ch < 0x80) {
            if (std::isalnum(ch)) {
                out.push_back(static_cast<char>(std::tolower(ch)));
                last_space = false;
            } else if (!last_space) {
                out.push_back(' ');
                last_space = true;
            }
        } else {
            out.push_back(static_cast<char>(ch));
            last_space = false;
        }
    }
    return trim(out);
}

static std::vector<std::string> split_normalized_tokens(const std::string& normalized) {
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < normalized.size()) {
        while (pos < normalized.size() && normalized[pos] == ' ') ++pos;
        if (pos >= normalized.size()) break;
        size_t end = pos;
        while (end < normalized.size() && normalized[end] != ' ') ++end;
        if (end > pos) tokens.push_back(normalized.substr(pos, end - pos));
        pos = end;
    }
    return tokens;
}

static double token_overlap_ratio(
    const std::string& expected_normalized,
    const std::string& actual_normalized) {

    const auto expected_tokens = split_normalized_tokens(expected_normalized);
    const auto actual_tokens = split_normalized_tokens(actual_normalized);
    if (expected_tokens.empty() || actual_tokens.empty()) return 0.0;
    std::unordered_set<std::string> expected_set(expected_tokens.begin(), expected_tokens.end());
    std::unordered_set<std::string> actual_set(actual_tokens.begin(), actual_tokens.end());
    size_t matched = 0;
    for (const auto& token : expected_set) {
        if (actual_set.find(token) != actual_set.end()) ++matched;
    }
    const size_t denominator = std::max(expected_set.size(), actual_set.size());
    return denominator == 0 ? 0.0 : static_cast<double>(matched) / static_cast<double>(denominator);
}

static bool strong_discogs_text_match(
    const std::string& expected,
    const std::string& actual) {

    const std::string expected_normalized = normalize_discogs_compare_text(expected);
    const std::string actual_normalized = normalize_discogs_compare_text(actual);
    if (expected_normalized.empty() || actual_normalized.empty()) return false;
    if (expected_normalized == actual_normalized) return true;
    return token_overlap_ratio(expected_normalized, actual_normalized) >= 0.90;
}

static bool is_various_artist(const std::string& artist) {
    const std::string normalized = normalize_discogs_compare_text(artist);
    return normalized == "various" ||
        normalized == "various artists" ||
        normalized == "v a" ||
        normalized == "va" ||
        normalized == "compilation";
}

static bool discogs_release_has_cd_format(JsonObject* release_obj) {
    JsonArray* formats = json_get_array_member(release_obj, "formats");
    if (!formats) return false;
    const guint len = json_array_get_length(formats);
    for (guint i = 0; i < len; ++i) {
        JsonObject* format = json_array_get_object_element(formats, i);
        if (!format) continue;
        const std::string name = normalize_discogs_compare_text(json_get_string_member(format, "name"));
        if (name == "cd") return true;
    }
    return false;
}

static std::vector<std::string> discogs_release_artist_candidates(JsonObject* release_obj) {
    std::vector<std::string> artists;
    const std::string artists_sort = strip_discogs_artist_suffix(
        json_get_string_member(release_obj, "artists_sort"));
    if (!artists_sort.empty()) artists.push_back(artists_sort);

    JsonArray* arr = json_get_array_member(release_obj, "artists");
    if (arr) {
        const guint len = json_array_get_length(arr);
        std::string joined;
        for (guint i = 0; i < len; ++i) {
            JsonObject* artist = json_array_get_object_element(arr, i);
            if (!artist) continue;
            const std::string name = strip_discogs_artist_suffix(json_get_string_member(artist, "name"));
            if (name.empty()) continue;
            artists.push_back(name);
            if (!joined.empty()) joined += " ";
            joined += name;
            const std::string join = json_get_string_member(artist, "join");
            if (!join.empty()) joined += join;
        }
        joined = trim(joined);
        if (!joined.empty()) artists.push_back(joined);
    }
    return artists;
}

static std::vector<std::string> discogs_release_track_titles(JsonObject* release_obj) {
    std::vector<std::string> titles;
    JsonArray* tracklist = json_get_array_member(release_obj, "tracklist");
    if (!tracklist) return titles;
    const guint len = json_array_get_length(tracklist);
    titles.reserve(len);
    for (guint i = 0; i < len; ++i) {
        JsonObject* track = json_array_get_object_element(tracklist, i);
        if (!track) continue;
        const std::string type = to_lower(json_get_string_member(track, "type_"));
        if (!type.empty() && type != "track") continue;
        const std::string title = trim(json_get_string_member(track, "title"));
        if (!title.empty()) titles.push_back(title);
    }
    return titles;
}

static bool is_digits_only(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char ch : value) {
        if (!std::isdigit(ch)) return false;
    }
    return true;
}

static bool is_generic_track_title(
    const std::string& title,
    size_t track_index_zero_based) {

    const std::string normalized = normalize_discogs_compare_text(title);
    if (normalized.empty()) return true;
    if (normalized == "untitled") return true;
    if (normalized == "track") return true;
    if (is_digits_only(normalized)) {
        try {
            return static_cast<size_t>(std::stoi(normalized)) == track_index_zero_based + 1;
        } catch (...) {
            return false;
        }
    }
    static constexpr const char* kPrefix = "track ";
    if (normalized.rfind(kPrefix, 0) == 0) {
        const std::string rest = trim(normalized.substr(std::strlen(kPrefix)));
        if (is_digits_only(rest)) {
            try {
                return static_cast<size_t>(std::stoi(rest)) == track_index_zero_based + 1;
            } catch (...) {
                return false;
            }
        }
    }
    return false;
}

static size_t expected_discogs_track_count(
    const CdRipCddbEntry* entry,
    const CdRipDiscToc* toc) {

    if (entry && entry->tracks_count > 0) return entry->tracks_count;
    if (toc && toc->tracks_count > 0) return toc->tracks_count;
    return 0;
}

static double cddb_discogs_track_overlap_ratio(
    const CdRipCddbEntry* entry,
    const std::vector<std::string>& discogs_titles) {

    if (!entry || !entry->tracks || entry->tracks_count == 0) return -1.0;
    const size_t count = std::min(entry->tracks_count, discogs_titles.size());
    size_t comparable = 0;
    size_t matched = 0;
    for (size_t i = 0; i < count; ++i) {
        const std::string cddb_title = track_tag(entry, i, "TITLE");
        if (is_generic_track_title(cddb_title, i)) continue;
        ++comparable;
        if (strong_discogs_text_match(cddb_title, discogs_titles[i])) {
            ++matched;
        }
    }

    const size_t expected_count = entry->tracks_count;
    const size_t comparable_threshold = std::max<size_t>(3, (expected_count + 1) / 2);
    if (comparable < comparable_threshold) return -1.0;
    return static_cast<double>(matched) / static_cast<double>(comparable);
}

static bool build_discogs_candidate_from_release_obj(
    const CdRipCddbEntry* entry,
    const CdRipDiscToc* toc,
    JsonObject* release_obj,
    bool strict_cddb_match,
    DiscogsCoverArtCandidate& candidate) {

    if (!release_obj) return false;
    const std::string image_url = select_discogs_image_url(release_obj);
    if (image_url.empty()) return false;

    const std::string release_title = json_get_string_member(release_obj, "title");
    const std::string release_id = json_get_string_member(release_obj, "id");
    std::vector<std::string> artist_candidates = discogs_release_artist_candidates(release_obj);
    const std::string artist_display = artist_candidates.empty() ? std::string{} : artist_candidates.front();

    double score = 0.0;
    if (strict_cddb_match) {
        const std::string album = trim(album_tag(entry, "ALBUM"));
        const std::string artist = trim(album_tag(entry, "ARTIST"));
        if (album.empty() || artist.empty()) return false;
        if (!discogs_release_has_cd_format(release_obj)) return false;
        if (!strong_discogs_text_match(album, release_title)) return false;
        score += normalize_discogs_compare_text(album) == normalize_discogs_compare_text(release_title)
            ? 50.0
            : 40.0;

        if (!is_various_artist(artist)) {
            bool artist_match = false;
            for (const auto& actual_artist : artist_candidates) {
                if (strong_discogs_text_match(artist, actual_artist)) {
                    artist_match = true;
                    break;
                }
            }
            if (!artist_match) return false;
            score += 30.0;
        }

        const std::vector<std::string> discogs_titles = discogs_release_track_titles(release_obj);
        const size_t expected_count = expected_discogs_track_count(entry, toc);
        if (expected_count > 0 && discogs_titles.size() != expected_count) return false;

        const double overlap = cddb_discogs_track_overlap_ratio(entry, discogs_titles);
        if (overlap >= 0.0) {
            if (overlap < 0.60) return false;
            score += overlap * 30.0;
        }
    } else {
        score += 100.0;
    }

    JsonObject* community = json_get_object_member(release_obj, "community");
    const int have_count = json_get_int_member(community, "have", 0);
    if (have_count > 0) {
        score += std::min(10.0, std::log1p(static_cast<double>(have_count)));
    }

    candidate.release_id = release_id;
    candidate.title = release_title;
    candidate.artist = artist_display;
    candidate.image_url = image_url;
    candidate.score = score;
    return true;
}

static bool http_get_discogs_image_bytes(
    const std::string& url,
    std::vector<uint8_t>& body,
    std::string& content_type,
    std::string& err) {

    HttpRetryPolicy policy{};
    policy.timeout_sec = kCoverArtTimeoutSec;
    policy.max_attempts = 3;
    policy.retry_delay_ms = kCoverArtRetryDelayMs;
    policy.max_redirects = 2;
    policy.respect_retry_after = true;

    return http_get_bytes_with_retry(
        "Discogs",
        url,
        cover_art_user_agent(),
        "image/*",
        policy,
        body,
        content_type,
        err);
}

static void emit_cover_art_activity(
    const CdRipActivityObserver* observer,
    void* callback_state,
    CdRipActivityStates activity_state,
    const char* source_label) {

    CdRipActivityInfo info{};
    info.phase = CDRIP_ACTIVITY_PHASE_COVER_ART_FETCH;
    info.state = activity_state;
    info.source_label = source_label;
    info.completed_sources = (activity_state == CDRIP_ACTIVITY_STATE_SOURCE_FINISHED
        || activity_state == CDRIP_ACTIVITY_STATE_PHASE_FINISHED) ? 1u : 0u;
    info.total_sources = 1;
    notify_activity(observer, callback_state, info);
}

}  // namespace

namespace cdrip::detail {
namespace {

static std::vector<DiscogsCoverArtCandidate> select_discogs_cover_art_candidates_from_release_jsons_impl(
    const CdRipCddbEntry* entry,
    const CdRipDiscToc* toc,
    const std::vector<std::string>& release_jsons,
    size_t max_candidates,
    bool strict_cddb_match,
    std::string& err) {

    err.clear();
    std::vector<DiscogsCoverArtCandidate> candidates;
    std::unordered_set<std::string> seen_releases;
    std::unordered_set<std::string> seen_images;

    for (const auto& release_json : release_jsons) {
        JsonParser* parser = json_parser_new();
        GError* gerr = nullptr;
        if (!json_parser_load_from_data(parser, release_json.c_str(), release_json.size(), &gerr)) {
            if (err.empty()) {
                err = gerr && gerr->message ? std::string{gerr->message} : "Discogs release parse error";
            }
            if (gerr) g_error_free(gerr);
            g_object_unref(parser);
            continue;
        }
        JsonNode* root = json_parser_get_root(parser);
        if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
            if (err.empty()) err = "Discogs release response is not a JSON object";
            g_object_unref(parser);
            continue;
        }

        DiscogsCoverArtCandidate candidate{};
        if (build_discogs_candidate_from_release_obj(
                entry,
                toc,
                json_node_get_object(root),
                strict_cddb_match,
                candidate)) {
            const std::string release_key = candidate.release_id.empty()
                ? candidate.title + "\n" + candidate.image_url
                : candidate.release_id;
            if (seen_releases.insert(release_key).second &&
                seen_images.insert(candidate.image_url).second) {
                candidates.push_back(candidate);
            }
        }
        g_object_unref(parser);
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const auto& lhs, const auto& rhs) {
            if (std::fabs(lhs.score - rhs.score) > 1e-9) {
                return lhs.score > rhs.score;
            }
            return lhs.release_id < rhs.release_id;
        });
    if (max_candidates > 0 && candidates.size() > max_candidates) {
        candidates.resize(max_candidates);
    }
    if (!candidates.empty()) err.clear();
    return candidates;
}

static std::string escape_discogs_query_value(const std::string& value) {
    gchar* escaped = g_uri_escape_string(value.c_str(), nullptr, true);
    if (!escaped) return {};
    std::string out = escaped;
    g_free(escaped);
    return out;
}

static std::string build_discogs_title_search_url(
    const std::string& artist,
    const std::string& album) {

    const std::string artist_encoded = escape_discogs_query_value(artist);
    const std::string album_encoded = escape_discogs_query_value(album);
    if (artist_encoded.empty() || album_encoded.empty()) return {};
    std::ostringstream oss;
    oss << "https://api.discogs.com/database/search"
        << "?type=release"
        << "&artist=" << artist_encoded
        << "&release_title=" << album_encoded
        << "&format=CD"
        << "&per_page=10";
    return oss.str();
}

static std::vector<std::string> extract_discogs_search_release_ids(
    const std::string& search_json,
    std::string& err) {

    std::vector<std::string> ids;
    err.clear();

    JsonParser* parser = json_parser_new();
    GError* gerr = nullptr;
    if (!json_parser_load_from_data(parser, search_json.c_str(), search_json.size(), &gerr)) {
        err = gerr && gerr->message ? std::string{gerr->message} : "Discogs search parse error";
        if (gerr) g_error_free(gerr);
        g_object_unref(parser);
        return ids;
    }
    JsonNode* root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        err = "Discogs search response is not a JSON object";
        g_object_unref(parser);
        return ids;
    }

    JsonArray* results = json_get_array_member(json_node_get_object(root), "results");
    if (results) {
        std::unordered_set<std::string> seen;
        const guint len = json_array_get_length(results);
        for (guint i = 0; i < len; ++i) {
            JsonObject* result = json_array_get_object_element(results, i);
            if (!result) continue;
            const std::string type = to_lower(json_get_string_member(result, "type"));
            if (!type.empty() && type != "release") continue;
            const std::string id = trim(json_get_string_member(result, "id"));
            if (id.empty()) continue;
            if (seen.insert(id).second) ids.push_back(id);
        }
    }
    g_object_unref(parser);
    return ids;
}

static bool fetch_discogs_release_json_by_id(
    const std::string& release_id,
    std::string& body,
    std::string& err) {

    if (release_id.empty()) return false;
    const std::string api_url = "https://api.discogs.com/releases/" + release_id;
    return http_get_discogs_json(api_url, body, err);
}

static bool fetch_discogs_candidate_image(
    const DiscogsCoverArtCandidate& candidate,
    CdRipCoverArt& art,
    std::string& err) {

    art = CdRipCoverArt{};
    std::vector<uint8_t> data;
    std::string content_type;
    if (!http_get_discogs_image_bytes(candidate.image_url, data, content_type, err)) {
        return false;
    }

    std::vector<uint8_t> normalized;
    std::string norm_err;
    const int max_width_px = g_cover_art_max_width.load(std::memory_order_relaxed);
    if (!normalize_image_to_png(data, max_width_px, normalized, norm_err)) {
        err = "Failed to normalize cover art image: " + norm_err;
        return false;
    }

    art.size = normalized.size();
    art.data = new uint8_t[art.size];
    std::copy(normalized.begin(), normalized.end(), const_cast<uint8_t*>(art.data));
    art.mime_type = make_cstr_copy("image/png");
    art.is_front = 1;
    art.available = 1;
    return true;
}

static void clear_discogs_cover_art(CdRipCoverArt& art) {
    if (art.data) {
        delete[] art.data;
        art.data = nullptr;
    }
    art.size = 0;
    release_cstr(art.mime_type);
    art.is_front = 0;
    art.available = 0;
}

}  // namespace

DiscogsCoverArtLookupMode discogs_cover_art_lookup_mode_for_entry(
    const CdRipCddbEntry* entry) {

    if (!entry) return DiscogsCoverArtLookupMode::NotApplicable;
    const std::string release_id = trim(album_tag(entry, "DISCOGS_RELEASE"));
    if (!release_id.empty()) return DiscogsCoverArtLookupMode::ReleaseId;

    const std::string source_label = to_lower(to_string_or_empty(entry->source_label));
    if (source_label == "musicbrainz") {
        return DiscogsCoverArtLookupMode::NotApplicable;
    }

    const std::string artist = trim(album_tag(entry, "ARTIST"));
    const std::string album = trim(album_tag(entry, "ALBUM"));
    if (!artist.empty() && !album.empty()) {
        return DiscogsCoverArtLookupMode::TitleSearch;
    }
    return DiscogsCoverArtLookupMode::NotApplicable;
}

std::vector<DiscogsCoverArtCandidate> select_discogs_cover_art_candidates_from_release_jsons(
    const CdRipCddbEntry* entry,
    const CdRipDiscToc* toc,
    const std::vector<std::string>& release_jsons,
    size_t max_candidates,
    std::string& err) {

    return select_discogs_cover_art_candidates_from_release_jsons_impl(
        entry,
        toc,
        release_jsons,
        max_candidates,
        true,
        err);
}

std::vector<DiscogsCoverArtImageCandidate> fetch_discogs_cover_art_image_candidates(
    const CdRipCddbEntry* entry,
    const CdRipDiscToc* toc,
    size_t max_candidates,
    std::string& err) {

    err.clear();
    std::vector<DiscogsCoverArtImageCandidate> out;
    if (!entry || max_candidates == 0) return out;

    std::vector<DiscogsCoverArtCandidate> candidates;
    const DiscogsCoverArtLookupMode mode = discogs_cover_art_lookup_mode_for_entry(entry);
    if (mode == DiscogsCoverArtLookupMode::ReleaseId) {
        std::string release_id = trim(album_tag(entry, "DISCOGS_RELEASE"));
        for (unsigned char ch : release_id) {
            if (!std::isdigit(ch)) {
                err = "Invalid DISCOGS_RELEASE tag value";
                return out;
            }
        }

        std::string release_json;
        if (!fetch_discogs_release_json_by_id(release_id, release_json, err)) {
            return out;
        }
        candidates = select_discogs_cover_art_candidates_from_release_jsons_impl(
            entry,
            toc,
            {release_json},
            1,
            false,
            err);
        if (candidates.empty() && err.empty()) {
            err = "Discogs release has no images";
        }
    } else if (mode == DiscogsCoverArtLookupMode::TitleSearch) {
        const std::string artist = trim(album_tag(entry, "ARTIST"));
        const std::string album = trim(album_tag(entry, "ALBUM"));
        const std::string search_url = build_discogs_title_search_url(artist, album);
        if (search_url.empty()) return out;

        std::string search_json;
        if (!http_get_discogs_json(search_url, search_json, err)) {
            return out;
        }
        std::string search_err;
        const std::vector<std::string> release_ids =
            extract_discogs_search_release_ids(search_json, search_err);
        if (!search_err.empty()) {
            err = search_err;
            return out;
        }

        std::vector<std::string> release_jsons;
        release_jsons.reserve(release_ids.size());
        std::string last_release_err;
        for (const auto& release_id : release_ids) {
            std::string release_json;
            std::string release_err;
            if (fetch_discogs_release_json_by_id(release_id, release_json, release_err)) {
                release_jsons.push_back(release_json);
            } else if (!release_err.empty()) {
                last_release_err = release_err;
            }
        }

        candidates = select_discogs_cover_art_candidates_from_release_jsons_impl(
            entry,
            toc,
            release_jsons,
            max_candidates,
            true,
            err);
        if (candidates.empty() && err.empty()) {
            err = last_release_err;
        }
    } else {
        return out;
    }

    std::string last_image_err;
    for (const auto& candidate : candidates) {
        DiscogsCoverArtImageCandidate image_candidate{};
        image_candidate.candidate = candidate;
        std::string image_err;
        if (fetch_discogs_candidate_image(candidate, image_candidate.art, image_err)) {
            out.push_back(image_candidate);
            if (out.size() >= max_candidates) break;
        } else if (!image_err.empty()) {
            last_image_err = image_err;
        }
    }
    if (out.empty() && !last_image_err.empty()) {
        err = last_image_err;
    }
    if (!out.empty()) err.clear();
    return out;
}

void release_discogs_cover_art_image_candidates(
    std::vector<DiscogsCoverArtImageCandidate>& candidates) {

    for (auto& candidate : candidates) {
        clear_discogs_cover_art(candidate.art);
    }
    candidates.clear();
}

}  // namespace cdrip::detail

extern "C" {

void cdrip_set_cover_art_max_width(
    int max_width_px) {

    if (max_width_px <= 0) max_width_px = kDefaultCoverArtMaxWidth;
    g_cover_art_max_width.store(max_width_px, std::memory_order_relaxed);
}

int cdrip_fetch_cover_art(
    CdRipCddbEntry* entry,
    const CdRipDiscToc* toc,
    const CdRipActivityObserver* observer,
    void* state,
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

    emit_cover_art_activity(
        observer,
        state,
        CDRIP_ACTIVITY_STATE_PHASE_STARTED,
        nullptr);
    emit_cover_art_activity(
        observer,
        state,
        CDRIP_ACTIVITY_STATE_SOURCE_STARTED,
        "coverartarchive");

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
        emit_cover_art_activity(
            observer,
            state,
            CDRIP_ACTIVITY_STATE_SOURCE_FINISHED,
            "coverartarchive");
        emit_cover_art_activity(
            observer,
            state,
            CDRIP_ACTIVITY_STATE_PHASE_FINISHED,
            nullptr);
        if (!err_msg.empty()) set_error(error, err_msg);
        return 0;
    }

    std::vector<uint8_t> normalized;
    std::string norm_err;
    const int max_width_px = g_cover_art_max_width.load(std::memory_order_relaxed);
    if (!normalize_image_to_png(data, max_width_px, normalized, norm_err)) {
        emit_cover_art_activity(
            observer,
            state,
            CDRIP_ACTIVITY_STATE_SOURCE_FINISHED,
            "coverartarchive");
        emit_cover_art_activity(
            observer,
            state,
            CDRIP_ACTIVITY_STATE_PHASE_FINISHED,
            nullptr);
        set_error(error, "Failed to normalize cover art image: " + norm_err);
        return 0;
    }

    content_type = "image/png";
    entry->cover_art.size = normalized.size();
    entry->cover_art.data = new uint8_t[entry->cover_art.size];
    std::copy(normalized.begin(), normalized.end(), const_cast<uint8_t*>(entry->cover_art.data));
    entry->cover_art.mime_type = make_cstr_copy(content_type);
    entry->cover_art.is_front = 1;
    entry->cover_art.available = 1;
    emit_cover_art_activity(
        observer,
        state,
        CDRIP_ACTIVITY_STATE_SOURCE_FINISHED,
        "coverartarchive");
    emit_cover_art_activity(
        observer,
        state,
        CDRIP_ACTIVITY_STATE_PHASE_FINISHED,
        nullptr);
    return 1;
}

int cdrip_fetch_discogs_cover_art(
    CdRipCddbEntry* entry,
    const CdRipDiscToc* toc,
    const CdRipActivityObserver* observer,
    void* state,
    const char** error) {

    clear_error(error);
    if (!entry) {
        set_error(error, "Invalid entry for Discogs cover art fetch");
        return 0;
    }

    const DiscogsCoverArtLookupMode mode = discogs_cover_art_lookup_mode_for_entry(entry);
    if (mode == DiscogsCoverArtLookupMode::NotApplicable) {
        return 0;
    }
    if (mode == DiscogsCoverArtLookupMode::ReleaseId) {
        const std::string release_id = trim(album_tag(entry, "DISCOGS_RELEASE"));
        for (unsigned char ch : release_id) {
            if (!std::isdigit(ch)) {
                set_error(error, "Invalid DISCOGS_RELEASE tag value");
                return 0;
            }
        }
    }

    emit_cover_art_activity(
        observer,
        state,
        CDRIP_ACTIVITY_STATE_PHASE_STARTED,
        nullptr);
    emit_cover_art_activity(
        observer,
        state,
        CDRIP_ACTIVITY_STATE_SOURCE_STARTED,
        "discogs");

    std::string err_msg;
    std::vector<DiscogsCoverArtImageCandidate> candidates =
        fetch_discogs_cover_art_image_candidates(entry, toc, 1, err_msg);
    if (candidates.empty()) {
        emit_cover_art_activity(
            observer,
            state,
            CDRIP_ACTIVITY_STATE_SOURCE_FINISHED,
            "discogs");
        emit_cover_art_activity(
            observer,
            state,
            CDRIP_ACTIVITY_STATE_PHASE_FINISHED,
            nullptr);
        if (!err_msg.empty()) set_error(error, err_msg);
        return 0;
    }

    if (entry->cover_art.data) {
        delete[] entry->cover_art.data;
        entry->cover_art.data = nullptr;
    }
    entry->cover_art.size = 0;
    release_cstr(entry->cover_art.mime_type);
    entry->cover_art = candidates[0].art;
    candidates[0].art = CdRipCoverArt{};
    release_discogs_cover_art_image_candidates(candidates);
    emit_cover_art_activity(
        observer,
        state,
        CDRIP_ACTIVITY_STATE_SOURCE_FINISHED,
        "discogs");
    emit_cover_art_activity(
        observer,
        state,
        CDRIP_ACTIVITY_STATE_PHASE_FINISHED,
        nullptr);
    return 1;
}

};
