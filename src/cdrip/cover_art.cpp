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
#include <vector>

#include <jpeglib.h>
#include <lcms2.h>
#include <png.h>

#include <glib.h>
#include <libsoup/soup.h>

#include "internal.h"
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

void cdrip_set_cover_art_max_width(
    int max_width_px) {

    if (max_width_px <= 0) max_width_px = kDefaultCoverArtMaxWidth;
    g_cover_art_max_width.store(max_width_px, std::memory_order_relaxed);
}

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

    std::vector<uint8_t> normalized;
    std::string norm_err;
    const int max_width_px = g_cover_art_max_width.load(std::memory_order_relaxed);
    if (!normalize_image_to_png(data, max_width_px, normalized, norm_err)) {
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
    return 1;
}

};
