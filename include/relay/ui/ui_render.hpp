#pragma once

// Lays out a scene's game interface for a view size and turns it into triangles: a draw list any
// renderer can draw (the Vulkan window during Run Game, the editor's preview through Dear ImGui,
// or the CPU rasterizer below for captures and tests). Everything here is plain engine code with
// no GPU or UI library dependency.

#include "relay/scene/scene.hpp"
#include "relay/ui/ui_font.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace relay {

// Screen-space vertex in view pixels. `color` is sRGB RGBA8 with straight alpha, red in the lowest
// byte; the drawn color is the vertex color times the texture sample.
struct UiVertex {
    float x{}, y{};
    float u{}, v{};
    std::uint32_t color{};
};

struct UiClipRect {
    float x0{}, y0{}, x1{}, y1{};
    auto operator<=>(const UiClipRect&) const = default;
};

struct UiDrawCommand {
    std::uint32_t texture{}; // Index into UiDrawList::textures.
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    UiClipRect clip;
};

struct UiDrawList {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<UiVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<UiDrawCommand> commands;
    std::vector<std::shared_ptr<const UiTexture>> textures;
    [[nodiscard]] bool empty() const { return commands.empty(); }
};

// Maps a control's own space (0,0 at its top-left, canvas units) to view pixels:
// view = (a * x + c * y + tx, b * x + d * y + ty).
struct UiAffine {
    double a{1.0}, b{}, c{}, d{1.0}, tx{}, ty{};
    [[nodiscard]] Vec2 apply(Vec2 point) const {
        return {a * point.x + c * point.y + tx, b * point.x + d * point.y + ty};
    }
    [[nodiscard]] UiAffine then(const UiAffine& inner) const; // this * inner
    [[nodiscard]] std::optional<UiAffine> inverse() const;
    // View pixels per control unit.
    [[nodiscard]] double scale() const;
};

// Every entity with its parent, so canvases are found through plain nodes, and its UI components.
struct UiSourceNode {
    Entity entity;
    Entity parent;
    const UiComponents* ui{}; // Null for nodes without UI; must outlive the layout.
    // Set when some of the node's widgets are switched off: `ui` then points at this copy without
    // them, and layouts built from the node keep it alive.
    std::shared_ptr<const UiComponents> owned;
};
[[nodiscard]] std::vector<UiSourceNode> ui_sources(const Scene& scene);

struct UiLayoutNode {
    Entity entity;
    const UiComponents* ui{};
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    std::size_t parent{none}; // The parent control, or none for canvases and top-level controls.
    std::vector<std::size_t> children;
    Vec2 position; // Top-left in the parent control's space (the canvas for top-level controls).
    Vec2 size;
    UiAffine transform;
    // The parent's space (where position, size and offsets are measured) to view pixels.
    UiAffine parent_transform;
    double opacity{1.0};
    bool visible{true};  // Also false when an ancestor or the canvas is hidden.
    UiClipRect clip;     // View pixels the control may draw in.
    std::int32_t layer{}; // Canvas sort order.
};

struct UiLayout {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<UiLayoutNode> nodes;
    std::vector<std::size_t> order; // Back to front: canvases by sort order, then depth first.
    std::unordered_map<std::uint64_t, std::size_t> by_entity;
    std::vector<std::shared_ptr<const UiComponents>> keep_alive; // Owners of nodes' filtered components.
    [[nodiscard]] const UiLayoutNode* find(Entity entity) const;
    // The node's rectangle's four corners in view pixels, clockwise from its top-left.
    [[nodiscard]] std::array<Vec2, 4> corners(const UiLayoutNode& node) const;
    // The topmost visible control under a view point that the pointer can reach (mouse filter not
    // ignore), passing over controls whose filter is pass when `blocking` is set. Null for none.
    [[nodiscard]] const UiLayoutNode* hit(Vec2 point, bool blocking = false) const;
    [[nodiscard]] bool contains(const UiLayoutNode& node, Vec2 point) const;
};

// Pointer state that changes how controls look.
struct UiVisualState {
    Entity hovered;
    Entity pressed; // Held down; a slider being dragged.
};

// Decoded images for image widgets, by project path, reloaded when their files change (checked
// at most once a second).
class UiImageCache {
public:
    static constexpr std::uintmax_t maximum_file_bytes = 64U * 1024U * 1024U;
    static constexpr std::uint32_t maximum_dimension = 8192U;
    void set_root(const std::filesystem::path& root);
    // Null when the file cannot be read or decoded; `error` says why.
    [[nodiscard]] std::shared_ptr<const UiTexture> image(std::string_view path, std::string* error = nullptr);

private:
    struct Entry {
        std::shared_ptr<UiTexture> texture;
        std::string error;
        std::filesystem::file_time_type time{};
        std::uintmax_t size{};
        std::chrono::steady_clock::time_point checked{};
    };
    std::filesystem::path root_;
    std::map<std::string, Entry, std::less<>> entries_;
};

// Lays out and draws interfaces. Holds the font and image caches, so keep one per drawing host.
// Not thread-safe.
class UiPainter {
public:
    // The project folder that font and image paths resolve against.
    void set_root(const std::filesystem::path& root);
    [[nodiscard]] UiLayout layout(const std::vector<UiSourceNode>& sources, std::uint32_t width,
                                  std::uint32_t height);
    [[nodiscard]] UiDrawList draw(const UiLayout& layout, const UiVisualState& state = {});
    // The smallest size a container gives the node, in canvas units: its min_size or what its
    // text, check box or children need, whichever is larger.
    [[nodiscard]] Vec2 minimum_size(const std::vector<UiSourceNode>& sources, Entity entity);
    // Files that could not be used, each reported once: "fonts/x.ttf: cannot read ...".
    [[nodiscard]] std::vector<std::string> take_warnings();
    [[nodiscard]] UiFontCache& fonts() { return fonts_; }
    [[nodiscard]] UiImageCache& images() { return images_; }

private:
    struct Build;
    void warn(std::string message);
    UiFontCache fonts_;
    UiImageCache images_;
    std::set<std::string, std::less<>> reported_;
    std::vector<std::string> warnings_;
};

// Blends a draw list over 8-bit sRGB RGBA pixels (width * height * 4, already holding the
// background), as the GPU does: vertex color times the bilinear texture sample, blended in linear
// light. Deterministic, for captures, previews and tests.
void rasterize_ui(const UiDrawList& list, std::vector<std::uint8_t>& rgba);

// Packs a color with an opacity into a UiVertex color.
[[nodiscard]] std::uint32_t pack_ui_color(const UiColor& color, double opacity = 1.0);

} // namespace relay
