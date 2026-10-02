#pragma once

// Fonts for the game interface: the built-in Inter (regular and bold, embedded in the engine) and
// project .ttf/.otf files, rasterized with stb_truetype into glyph atlas pages as they are first
// drawn. Glyphs are rendered at the exact pixel size they appear on screen, so text stays sharp
// at every canvas scale. Not thread-safe; each drawing host keeps its own cache.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace relay {

struct EmbeddedFile {
    std::string_view name;
    std::span<const unsigned char> bytes;
};
// Files built into the engine: Inter-Regular.ttf, Inter-Bold.ttf and relay-icon.png (window icon).
[[nodiscard]] const std::vector<EmbeddedFile>& embedded_files();

// Pixels a renderer uploads as a texture: 8-bit RGBA, sRGB with straight alpha. `revision` changes
// whenever the pixels do; `id` is unique for the life of the process.
struct UiTexture {
    std::uint64_t id{};
    std::uint64_t revision{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};
[[nodiscard]] std::uint64_t next_ui_texture_id();

struct UiGlyph {
    // The quad relative to the pen on the baseline, in pixels, and its atlas coordinates.
    float x0{}, y0{}, x1{}, y1{};
    float u0{}, v0{}, u1{}, v1{};
    float advance{};
    std::uint32_t page{};
    bool visible{}; // False for spaces and glyphs without outlines.
};

struct UiFontMetrics {
    float ascent{};   // Above the baseline, pixels.
    float descent{};  // Below the baseline, positive.
    float line_height{};
};

// One laid-out glyph of a text block, positioned on its line.
struct UiPlacedGlyph {
    std::uint32_t codepoint{};
    std::uint32_t font{}; // The font that has it (a fallback when the chosen font lacks it).
    float x{};            // Pen position from the line's start, pixels.
    std::uint32_t line{};
};

struct UiTextLayout {
    std::vector<UiPlacedGlyph> glyphs;
    std::vector<float> line_widths;
    float line_height{};  // Distance between baselines, with line spacing.
    float ascent{};
    float width{};        // Widest line.
    float height{};       // All lines: (lines - 1) * line_height + ascent + descent.
};

class UiFontCache {
public:
    static constexpr std::uint32_t page_size = 1024U;
    static constexpr std::size_t maximum_pages = 8U;
    static constexpr std::uintmax_t maximum_font_bytes = 32U * 1024U * 1024U;

    UiFontCache();
    ~UiFontCache();
    UiFontCache(const UiFontCache&) = delete;
    UiFontCache& operator=(const UiFontCache&) = delete;

    // Where project font paths resolve. Changing it forgets loaded project fonts.
    void set_root(const std::filesystem::path& root);
    // A font handle: `path` under the root, or the built-in font (bold or regular) when empty or
    // unreadable, in which case `error` says why.
    [[nodiscard]] std::uint32_t font(std::string_view path, bool bold, std::string* error = nullptr);
    [[nodiscard]] UiFontMetrics metrics(std::uint32_t font, float pixels);
    // Rasterizes on first use. Fonts without the character fall back to the built-in font, whose
    // glyph is returned; `font` then names the font that drew it.
    [[nodiscard]] const UiGlyph& glyph(std::uint32_t& font, float pixels, std::uint32_t codepoint);
    [[nodiscard]] float advance(std::uint32_t& font, float pixels, std::uint32_t codepoint);
    [[nodiscard]] float kerning(std::uint32_t font, float pixels, std::uint32_t left, std::uint32_t right);
    // Lays out UTF-8 text at a pixel size, breaking at new lines and, when `wrap_width` is
    // positive, between words (or inside words longer than a line).
    [[nodiscard]] UiTextLayout layout(std::uint32_t font, float pixels, std::string_view text,
                                      float wrap_width, float line_spacing);

    // Atlas pages; page 0 has a white block at `white_uv` for solid shapes.
    [[nodiscard]] const std::vector<std::shared_ptr<UiTexture>>& pages() const { return pages_; }
    [[nodiscard]] std::array<float, 2> white_uv() const;
    // Drops every glyph when the atlas is full, so the next frame rasterizes what it needs again.
    // Call between frames; glyph references do not survive it.
    void trim();

private:
    struct Face;
    struct GlyphKey {
        std::uint32_t font;
        std::uint32_t pixels; // Rounded pixel size; sizes below 1 draw at 1.
        std::uint32_t codepoint;
        bool operator==(const GlyphKey&) const = default;
    };
    struct GlyphHash {
        std::size_t operator()(const GlyphKey& key) const {
            return (static_cast<std::size_t>(key.font) * 1000003U) ^ (static_cast<std::size_t>(key.pixels) << 21U) ^
                   key.codepoint;
        }
    };
    [[nodiscard]] Face* face(std::uint32_t font);
    [[nodiscard]] bool has_glyph(std::uint32_t font, std::uint32_t codepoint);
    bool allocate(std::uint32_t width, std::uint32_t height, std::uint32_t& page, std::uint32_t& x,
                  std::uint32_t& y);
    void reset_pages();

    std::filesystem::path root_;
    std::vector<std::unique_ptr<Face>> faces_; // 0 regular, 1 bold, then project fonts.
    std::map<std::string, std::uint32_t, std::less<>> by_path_;
    std::unordered_map<GlyphKey, UiGlyph, GlyphHash> glyphs_;
    std::vector<std::shared_ptr<UiTexture>> pages_;
    std::uint32_t shelf_x_{}, shelf_y_{}, shelf_height_{};
    bool full_{};
};

// Decodes one UTF-8 code point at `offset`, advancing it; malformed bytes read as U+FFFD.
[[nodiscard]] std::uint32_t decode_utf8(std::string_view text, std::size_t& offset);

} // namespace relay
