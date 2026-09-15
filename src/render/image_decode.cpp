#include "relay/render/image_decode.hpp"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <limits>

#ifdef RELAY_HAS_PNG
#include <png.h>
#endif

#ifdef RELAY_HAS_JPEG
extern "C" {
#include <jpeglib.h>
}
#endif

namespace relay {
namespace {

constexpr std::uint32_t maximum_dimension = 8192U;
constexpr std::size_t maximum_decoded_bytes = 256U * 1024U * 1024U;

bool dimensions_are_safe(const std::uint32_t width, const std::uint32_t height) {
    if (width == 0U || height == 0U || width > maximum_dimension || height > maximum_dimension) {
        return false;
    }
    return static_cast<std::uint64_t>(width) * height * 4U <= maximum_decoded_bytes;
}

bool looks_like_png(const std::span<const std::uint8_t> bytes) {
    static constexpr std::array<std::uint8_t, 8> signature{137U, 80U, 78U, 71U,
                                                           13U, 10U, 26U, 10U};
    return bytes.size() >= signature.size() &&
           std::equal(signature.begin(), signature.end(), bytes.begin());
}

bool looks_like_jpeg(const std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 3U && bytes[0] == 0xFFU && bytes[1] == 0xD8U && bytes[2] == 0xFFU;
}

#ifdef RELAY_HAS_PNG
bool decode_png(const std::span<const std::uint8_t> bytes, TextureAsset& output,
                std::string& error) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()) == 0) {
        error = "PNG header could not be decoded";
        return false;
    }
    if (!dimensions_are_safe(image.width, image.height)) {
        png_image_free(&image);
        error = "decoded PNG dimensions exceed the image limit";
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
    if (png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) == 0) {
        error = image.message[0] == '\0' ? "PNG pixels could not be decoded" : image.message;
        png_image_free(&image);
        return false;
    }
    output.width = image.width;
    output.height = image.height;
    output.rgba = std::move(pixels);
    png_image_free(&image);
    return true;
}
#endif

#ifdef RELAY_HAS_JPEG
struct JpegError {
    jpeg_error_mgr base{};
    std::jmp_buf jump{};
    char message[JMSG_LENGTH_MAX]{};
    std::uint8_t* pixels{};
    std::uint8_t* row{};
};

void jpeg_failure(j_common_ptr decoder) {
    auto* state = reinterpret_cast<JpegError*>(decoder->err);
    (*decoder->err->format_message)(decoder, state->message);
    std::longjmp(state->jump, 1);
}

bool decode_jpeg(const std::span<const std::uint8_t> bytes, TextureAsset& output,
                 std::string& error) {
    jpeg_decompress_struct decoder{};
    JpegError failure{};
    decoder.err = jpeg_std_error(&failure.base);
    failure.base.error_exit = jpeg_failure;
    if (setjmp(failure.jump) != 0) {
        std::free(failure.pixels);
        std::free(failure.row);
        jpeg_destroy_decompress(&decoder);
        error = failure.message[0] == '\0' ? "JPEG could not be decoded" : failure.message;
        return false;
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, bytes.data(), static_cast<unsigned long>(bytes.size()));
    (void)jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_RGB;
    (void)jpeg_start_decompress(&decoder);
    const auto width = static_cast<std::uint32_t>(decoder.output_width);
    const auto height = static_cast<std::uint32_t>(decoder.output_height);
    if (!dimensions_are_safe(width, height)) {
        jpeg_destroy_decompress(&decoder);
        error = "decoded JPEG dimensions exceed the image limit";
        return false;
    }
    const auto pixel_bytes = static_cast<std::size_t>(width) * height * 4U;
    failure.pixels = static_cast<std::uint8_t*>(std::malloc(pixel_bytes));
    if (failure.pixels == nullptr) {
        jpeg_destroy_decompress(&decoder);
        error = "could not allocate decoded JPEG pixels";
        return false;
    }
    failure.row = static_cast<std::uint8_t*>(
        std::malloc(static_cast<std::size_t>(width) * 3U));
    if (failure.row == nullptr) {
        std::free(failure.pixels);
        failure.pixels = nullptr;
        jpeg_destroy_decompress(&decoder);
        error = "could not allocate a JPEG scanline";
        return false;
    }
    while (decoder.output_scanline < decoder.output_height) {
        JSAMPROW rows[]{failure.row};
        (void)jpeg_read_scanlines(&decoder, rows, 1U);
        const auto y = static_cast<std::size_t>(decoder.output_scanline - 1U);
        auto* destination = failure.pixels + y * static_cast<std::size_t>(width) * 4U;
        for (std::size_t x = 0; x < width; ++x) {
            destination[x * 4U] = failure.row[x * 3U];
            destination[x * 4U + 1U] = failure.row[x * 3U + 1U];
            destination[x * 4U + 2U] = failure.row[x * 3U + 2U];
            destination[x * 4U + 3U] = 255U;
        }
    }
    (void)jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    output.width = width;
    output.height = height;
    output.rgba.assign(failure.pixels, failure.pixels + pixel_bytes);
    std::free(failure.pixels);
    std::free(failure.row);
    failure.pixels = nullptr;
    failure.row = nullptr;
    return true;
}
#endif

} // namespace

bool decode_image_rgba(const std::span<const std::uint8_t> bytes,
                       const std::string_view format_hint, TextureAsset& output,
                       std::string& error) {
    error.clear();
    if (bytes.empty()) {
        error = "image payload is empty";
        return false;
    }
    const bool png = looks_like_png(bytes) || format_hint == "png" || format_hint == ".png";
    const bool jpeg = looks_like_jpeg(bytes) || format_hint == "jpg" || format_hint == "jpeg" ||
                      format_hint == ".jpg" || format_hint == ".jpeg";
    if (png) {
#ifdef RELAY_HAS_PNG
        return decode_png(bytes, output, error);
#else
        error = "Relay was built without PNG decoding support";
        return false;
#endif
    }
    if (jpeg) {
#ifdef RELAY_HAS_JPEG
        return decode_jpeg(bytes, output, error);
#else
        error = "Relay was built without JPEG decoding support";
        return false;
#endif
    }
    error = "image format is not PNG or JPEG";
    return false;
}

} // namespace relay
