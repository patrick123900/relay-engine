#include "relay/ui/ui_font.hpp"

#include "relay/scene/project.hpp"

#include "stb_truetype.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>

namespace relay {

std::uint64_t next_ui_texture_id() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1U);
}

std::uint32_t decode_utf8(const std::string_view text, std::size_t& offset) {
    constexpr std::uint32_t replacement = 0xFFFDU;
    const auto byte = [&](std::size_t index) { return static_cast<std::uint8_t>(text[index]); };
    const auto lead = byte(offset++);
    if (lead < 0x80U) return lead;
    std::size_t extra = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xE0U) == 0xC0U) {
        extra = 1;
        codepoint = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
        extra = 2;
        codepoint = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
        extra = 3;
        codepoint = lead & 0x07U;
    } else {
        return replacement;
    }
    for (std::size_t index = 0; index < extra; ++index) {
        if (offset >= text.size() || (byte(offset) & 0xC0U) != 0x80U) return replacement;
        codepoint = (codepoint << 6U) | (byte(offset++) & 0x3FU);
    }
    static constexpr std::uint32_t smallest[] = {0U, 0x80U, 0x800U, 0x10000U};
    if (codepoint < smallest[extra] || codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU))
        return replacement;
    return codepoint;
}

struct UiFontCache::Face {
    std::string path; // Empty for built-in fonts.
    std::vector<unsigned char> owned;
    const unsigned char* data{};
    stbtt_fontinfo info{};
    bool valid{};
    std::filesystem::file_time_type time{};
    std::uintmax_t size{};
    std::chrono::steady_clock::time_point checked{};

    bool load(const unsigned char* bytes, std::size_t length) {
        data = bytes;
        const int offset = stbtt_GetFontOffsetForIndex(bytes, 0);
        valid = length >= 12U && offset >= 0 && stbtt_InitFont(&info, bytes, offset) != 0;
        return valid;
    }
    [[nodiscard]] float scale(float pixels) const { return stbtt_ScaleForMappingEmToPixels(&info, pixels); }
};

namespace {

// The built-in fonts, parsed once per cache from the engine's embedded copies.
std::span<const unsigned char> embedded(const std::string_view name) {
    for (const auto& file : embedded_files())
        if (file.name == name) return file.bytes;
    return {};
}

bool read_font_file(const std::filesystem::path& path, std::vector<unsigned char>& bytes, std::string& error) {
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code || !std::filesystem::is_regular_file(path, code)) {
        error = "cannot read font " + path.filename().string();
        return false;
    }
    if (size > UiFontCache::maximum_font_bytes) {
        error = "font " + path.filename().string() + " is larger than 32 MiB";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    bytes.resize(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        error = "cannot read font " + path.filename().string();
        return false;
    }
    return true;
}

constexpr std::uint32_t atlas_padding = 1U;
constexpr std::uint32_t white_block = 4U;

} // namespace

UiFontCache::UiFontCache() {
    for (const auto* name : {"Inter-Regular.ttf", "Inter-Bold.ttf"}) {
        auto face = std::make_unique<Face>();
        const auto bytes = embedded(name);
        (void)face->load(bytes.data(), bytes.size());
        faces_.push_back(std::move(face));
    }
    reset_pages();
}

UiFontCache::~UiFontCache() = default;

void UiFontCache::set_root(const std::filesystem::path& root) {
    if (root == root_) return;
    root_ = root;
    // Handles of project fonts become invalid; built-ins stay 0 and 1.
    faces_.resize(2U);
    by_path_.clear();
    glyphs_.clear();
    reset_pages();
}

void UiFontCache::reset_pages() {
    for (auto& page : pages_) {
        // Keep texture identities so renderers replace pixels rather than textures.
        std::fill(page->rgba.begin(), page->rgba.end(), std::uint8_t{0});
        ++page->revision;
    }
    if (pages_.empty()) {
        auto page = std::make_shared<UiTexture>();
        page->id = next_ui_texture_id();
        page->revision = 1U;
        page->width = page->height = page_size;
        page->rgba.assign(static_cast<std::size_t>(page_size) * page_size * 4U, 0U);
        pages_.push_back(std::move(page));
    }
    pages_.resize(1U);
    // Solid shapes sample the middle of a white block in page 0.
    auto& first = *pages_.front();
    for (std::uint32_t y = 0; y < white_block; ++y)
        for (std::uint32_t x = 0; x < white_block; ++x) {
            const auto at = (static_cast<std::size_t>(y) * page_size + x) * 4U;
            first.rgba[at] = first.rgba[at + 1U] = first.rgba[at + 2U] = first.rgba[at + 3U] = 255U;
        }
    shelf_x_ = white_block + atlas_padding;
    shelf_y_ = 0U;
    shelf_height_ = white_block;
    full_ = false;
}

std::array<float, 2> UiFontCache::white_uv() const {
    const float middle = static_cast<float>(white_block) * 0.5F / static_cast<float>(page_size);
    return {middle, middle};
}

void UiFontCache::trim() {
    if (!full_) return;
    glyphs_.clear();
    reset_pages();
}

bool UiFontCache::allocate(const std::uint32_t width, const std::uint32_t height, std::uint32_t& page,
                           std::uint32_t& x, std::uint32_t& y) {
    if (width + atlas_padding > page_size || height + atlas_padding > page_size) return false;
    if (shelf_x_ + width + atlas_padding > page_size) {
        shelf_y_ += shelf_height_ + atlas_padding;
        shelf_x_ = 0U;
        shelf_height_ = 0U;
    }
    if (shelf_y_ + height + atlas_padding > page_size) {
        if (pages_.size() >= maximum_pages) {
            full_ = true;
            return false;
        }
        auto next = std::make_shared<UiTexture>();
        next->id = next_ui_texture_id();
        next->revision = 1U;
        next->width = next->height = page_size;
        next->rgba.assign(static_cast<std::size_t>(page_size) * page_size * 4U, 0U);
        pages_.push_back(std::move(next));
        shelf_x_ = shelf_y_ = shelf_height_ = 0U;
    }
    page = static_cast<std::uint32_t>(pages_.size() - 1U);
    x = shelf_x_;
    y = shelf_y_;
    shelf_x_ += width + atlas_padding;
    shelf_height_ = std::max(shelf_height_, height);
    return true;
}

UiFontCache::Face* UiFontCache::face(const std::uint32_t font) {
    return font < faces_.size() && faces_[font]->valid ? faces_[font].get() : faces_.front().get();
}

std::uint32_t UiFontCache::font(const std::string_view path, const bool bold, std::string* error) {
    const std::uint32_t builtin = bold ? 1U : 0U;
    if (path.empty()) return builtin;
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return builtin;
    };
    const auto now = std::chrono::steady_clock::now();
    const auto found = by_path_.find(path);
    // Files are checked for changes at most once a second.
    if (found != by_path_.end() && now - faces_[found->second]->checked < std::chrono::seconds(1))
        return faces_[found->second]->valid ? found->second : fail("font " + std::string(path) + " cannot be read");
    // Project paths only: no traversal, hidden folders or symbolic links out of the project.
    const auto checked = workspace_file(root_.generic_string(), path, "");
    if (!checked) return fail("font path " + std::string(path) + " is not a project file");
    const auto& full_path = *checked;
    if (found != by_path_.end()) {
        auto& existing = *faces_[found->second];
        existing.checked = now;
        std::error_code code;
        const auto time = std::filesystem::last_write_time(full_path, code);
        const auto size = code ? 0U : std::filesystem::file_size(full_path, code);
        if (!code && time == existing.time && size == existing.size)
            return existing.valid ? found->second : fail("font " + std::string(path) + " cannot be read");
        // Changed: forget its glyphs (their atlas space stays used until the atlas next fills).
        std::erase_if(glyphs_, [&](const auto& entry) { return entry.first.font == found->second; });
    }
    auto loaded = std::make_unique<Face>();
    loaded->path = std::string(path);
    loaded->checked = now;
    std::error_code code;
    loaded->time = std::filesystem::last_write_time(full_path, code);
    loaded->size = code ? 0U : std::filesystem::file_size(full_path, code);
    std::string reason;
    const bool read = read_font_file(full_path, loaded->owned, reason);
    if (read && !loaded->load(loaded->owned.data(), loaded->owned.size()))
        reason = "font " + std::string(path) + " is not a TrueType or OpenType font";
    std::uint32_t handle{};
    if (found != by_path_.end()) {
        handle = found->second;
        faces_[handle] = std::move(loaded);
    } else {
        handle = static_cast<std::uint32_t>(faces_.size());
        faces_.push_back(std::move(loaded));
        by_path_.emplace(std::string(path), handle);
    }
    return faces_[handle]->valid ? handle : fail(reason);
}

bool UiFontCache::has_glyph(const std::uint32_t font, const std::uint32_t codepoint) {
    auto* selected = face(font);
    return stbtt_FindGlyphIndex(&selected->info, static_cast<int>(codepoint)) != 0;
}

UiFontMetrics UiFontCache::metrics(const std::uint32_t font, const float pixels) {
    auto* selected = face(font);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&selected->info, &ascent, &descent, &gap);
    const float scale = selected->scale(pixels);
    UiFontMetrics result;
    result.ascent = static_cast<float>(ascent) * scale;
    result.descent = static_cast<float>(-descent) * scale;
    result.line_height = static_cast<float>(ascent - descent + gap) * scale;
    return result;
}

const UiGlyph& UiFontCache::glyph(std::uint32_t& font, const float pixels, const std::uint32_t codepoint) {
    if (font >= faces_.size() || !faces_[font]->valid) font = 0U;
    // A project font without the character borrows the built-in font's.
    if (font > 1U && !has_glyph(font, codepoint) && has_glyph(0U, codepoint)) font = 0U;
    const auto rounded = static_cast<std::uint32_t>(std::max(1L, std::lround(pixels)));
    const GlyphKey key{font, rounded, codepoint};
    if (const auto found = glyphs_.find(key); found != glyphs_.end()) return found->second;
    auto* selected = face(font);
    const float scale = selected->scale(static_cast<float>(rounded));
    const int index = stbtt_FindGlyphIndex(&selected->info, static_cast<int>(codepoint));
    int advance = 0, bearing = 0;
    stbtt_GetGlyphHMetrics(&selected->info, index, &advance, &bearing);
    UiGlyph result;
    result.advance = static_cast<float>(advance) * scale;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&selected->info, index, scale, scale, &x0, &y0, &x1, &y1);
    const int width = x1 - x0;
    const int height = y1 - y0;
    std::uint32_t page = 0, x = 0, y = 0;
    if (width > 0 && height > 0 && codepoint != ' ' &&
        allocate(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), page, x, y)) {
        std::vector<unsigned char> coverage(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        stbtt_MakeGlyphBitmap(&selected->info, coverage.data(), width, height, width, scale, scale, index);
        auto& texture = *pages_[page];
        for (int row = 0; row < height; ++row)
            for (int column = 0; column < width; ++column) {
                const auto at = ((static_cast<std::size_t>(y) + static_cast<std::size_t>(row)) * page_size + x +
                                 static_cast<std::size_t>(column)) * 4U;
                texture.rgba[at] = texture.rgba[at + 1U] = texture.rgba[at + 2U] = 255U;
                texture.rgba[at + 3U] =
                    coverage[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(column)];
            }
        ++texture.revision;
        const float size = static_cast<float>(page_size);
        result.x0 = static_cast<float>(x0);
        result.y0 = static_cast<float>(y0);
        result.x1 = static_cast<float>(x1);
        result.y1 = static_cast<float>(y1);
        result.u0 = static_cast<float>(x) / size;
        result.v0 = static_cast<float>(y) / size;
        result.u1 = static_cast<float>(x + static_cast<std::uint32_t>(width)) / size;
        result.v1 = static_cast<float>(y + static_cast<std::uint32_t>(height)) / size;
        result.page = page;
        result.visible = true;
    }
    return glyphs_.emplace(key, result).first->second;
}

float UiFontCache::advance(std::uint32_t& font, const float pixels, const std::uint32_t codepoint) {
    if (font >= faces_.size() || !faces_[font]->valid) font = 0U;
    if (font > 1U && !has_glyph(font, codepoint) && has_glyph(0U, codepoint)) font = 0U;
    auto* selected = face(font);
    const int index = stbtt_FindGlyphIndex(&selected->info, static_cast<int>(codepoint));
    int advance = 0, bearing = 0;
    stbtt_GetGlyphHMetrics(&selected->info, index, &advance, &bearing);
    return static_cast<float>(advance) * selected->scale(pixels);
}

float UiFontCache::kerning(const std::uint32_t font, const float pixels, const std::uint32_t left,
                           const std::uint32_t right) {
    auto* selected = face(font);
    return static_cast<float>(stbtt_GetCodepointKernAdvance(&selected->info, static_cast<int>(left),
                                                            static_cast<int>(right))) *
           selected->scale(pixels);
}

UiTextLayout UiFontCache::layout(const std::uint32_t font, const float pixels, const std::string_view text,
                                 const float wrap_width, const float line_spacing) {
    struct Item {
        std::uint32_t codepoint;
        std::uint32_t font;
        float advance;
    };
    std::vector<std::vector<Item>> lines(1U);
    const auto width_of = [&](const std::vector<Item>& items) {
        float total = 0.0F;
        for (std::size_t index = 0; index < items.size(); ++index) {
            total += items[index].advance;
            if (index > 0U && items[index - 1U].font == items[index].font)
                total += kerning(items[index].font, pixels, items[index - 1U].codepoint, items[index].codepoint);
        }
        return total;
    };
    for (std::size_t offset = 0; offset < text.size();) {
        const auto codepoint = decode_utf8(text, offset);
        if (codepoint == '\r') continue;
        if (codepoint == '\n') {
            lines.emplace_back();
            continue;
        }
        const auto chosen = codepoint == '\t' ? std::uint32_t{' '} : codepoint;
        std::uint32_t used = font;
        const float step = advance(used, pixels, chosen) * (codepoint == '\t' ? 4.0F : 1.0F);
        auto& line = lines.back();
        line.push_back({chosen, used, step});
        if (wrap_width <= 0.0F || line.size() < 2U || chosen == ' ' || width_of(line) <= wrap_width) continue;
        // Too long: break after the last space, or before this character inside a long word.
        std::size_t space = line.size();
        for (std::size_t index = line.size() - 1U; index-- > 0U;)
            if (line[index].codepoint == ' ') {
                space = index;
                break;
            }
        std::vector<Item> next;
        if (space < line.size()) {
            next.assign(line.begin() + static_cast<std::ptrdiff_t>(space) + 1, line.end());
            line.erase(line.begin() + static_cast<std::ptrdiff_t>(space), line.end());
        } else {
            next.push_back(line.back());
            line.pop_back();
        }
        lines.push_back(std::move(next));
    }
    const auto measure = metrics(font, pixels);
    UiTextLayout result;
    result.ascent = measure.ascent;
    result.line_height = measure.line_height * line_spacing;
    for (std::size_t number = 0; number < lines.size(); ++number) {
        auto& line = lines[number];
        // Trailing spaces neither count towards alignment nor draw.
        while (!line.empty() && line.back().codepoint == ' ') line.pop_back();
        float pen = 0.0F;
        for (std::size_t index = 0; index < line.size(); ++index) {
            if (index > 0U && line[index - 1U].font == line[index].font)
                pen += kerning(line[index].font, pixels, line[index - 1U].codepoint, line[index].codepoint);
            result.glyphs.push_back({line[index].codepoint, line[index].font, pen, static_cast<std::uint32_t>(number)});
            pen += line[index].advance;
        }
        result.line_widths.push_back(pen);
        result.width = std::max(result.width, pen);
    }
    result.height = static_cast<float>(lines.size() - 1U) * result.line_height + measure.ascent + measure.descent;
    return result;
}

} // namespace relay
