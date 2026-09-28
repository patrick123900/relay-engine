#include "relay/ui/ui_render.hpp"

#include "relay/render/image_decode.hpp"
#include "relay/scene/project.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <utility>

namespace relay {
namespace {

UiAffine translation(double x, double y) { return {1.0, 0.0, 0.0, 1.0, x, y}; }
UiAffine rotation(double degrees) {
    const double radians = degrees * std::numbers::pi / 180.0;
    const double cosine = std::cos(radians), sine = std::sin(radians);
    // View y points down, so positive angles turn clockwise on screen.
    return {cosine, sine, -sine, cosine, 0.0, 0.0};
}
UiAffine scaling(double x, double y) { return {x, 0.0, 0.0, y, 0.0, 0.0}; }

UiClipRect intersect(const UiClipRect& a, const UiClipRect& b) {
    UiClipRect result{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    result.x1 = std::max(result.x1, result.x0);
    result.y1 = std::max(result.y1, result.y0);
    return result;
}

Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 subtract(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }

double placement_factor(UiControl::Placement placement) {
    switch (placement) {
    case UiControl::Placement::center: return 0.5;
    case UiControl::Placement::end: return 1.0;
    default: return 0.0;
    }
}

double align_factor(UiContainer::Align align) {
    return align == UiContainer::Align::center ? 0.5 : align == UiContainer::Align::end ? 1.0 : 0.0;
}

double text_factor(UiLabel::Align align) {
    return align == UiLabel::Align::center ? 0.5 : align == UiLabel::Align::end ? 1.0 : 0.0;
}

// How a control sits in a container cell: the whole cell, or its minimum size placed at the
// cell's start, center or end, per axis.
void fit_in_cell(const UiControl& control, Vec2 cell_position, Vec2 cell_size, Vec2 minimum, Vec2& position,
                 Vec2& size) {
    const auto axis = [](UiControl::Placement placement, double start, double extent, double least, double& at,
                         double& length) {
        if (placement == UiControl::Placement::fill) {
            at = start;
            length = extent;
            return;
        }
        length = std::min(least, extent);
        at = start + (extent - length) * placement_factor(placement);
    };
    axis(control.size_x, cell_position.x, cell_size.x, minimum.x, position.x, size.x);
    axis(control.size_y, cell_position.y, cell_size.y, minimum.y, position.y, size.y);
}

double canvas_scale(const UiCanvas& canvas, std::uint32_t width, std::uint32_t height) {
    if (canvas.scale_mode == UiCanvas::ScaleMode::constant_pixel_size) return canvas.scale;
    const double by_width = std::log(std::max(static_cast<double>(width), 1.0) / canvas.reference_size.x);
    const double by_height = std::log(std::max(static_cast<double>(height), 1.0) / canvas.reference_size.y);
    return std::exp(by_width + (by_height - by_width) * canvas.match);
}

std::uint8_t to_byte(double value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

UiColor mix(const UiColor& a, const UiColor& b, double amount) {
    return {a.r + (b.r - a.r) * amount, a.g + (b.g - a.g) * amount, a.b + (b.b - a.b) * amount,
            a.a + (b.a - a.a) * amount};
}

UiColor multiply(const UiColor& a, const UiColor& b) { return {a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a}; }

} // namespace

std::uint32_t pack_ui_color(const UiColor& color, const double opacity) {
    return static_cast<std::uint32_t>(to_byte(color.r)) | (static_cast<std::uint32_t>(to_byte(color.g)) << 8U) |
           (static_cast<std::uint32_t>(to_byte(color.b)) << 16U) |
           (static_cast<std::uint32_t>(to_byte(color.a * opacity)) << 24U);
}

UiAffine UiAffine::then(const UiAffine& m) const {
    return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d, a * m.tx + c * m.ty + tx,
            b * m.tx + d * m.ty + ty};
}

std::optional<UiAffine> UiAffine::inverse() const {
    const double determinant = a * d - b * c;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) return std::nullopt;
    const double ia = d / determinant, ib = -b / determinant, ic = -c / determinant, id = a / determinant;
    return UiAffine{ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)};
}

double UiAffine::scale() const { return std::sqrt(std::abs(a * d - b * c)); }

std::vector<UiSourceNode> ui_sources(const Scene& scene) {
    std::vector<UiSourceNode> sources;
    for (const auto entity : scene.entities()) {
        const auto* record = scene.get(entity);
        sources.push_back({entity, record->parent, record->ui.empty() ? nullptr : &record->ui});
    }
    return sources;
}

const UiLayoutNode* UiLayout::find(const Entity entity) const {
    const auto found = by_entity.find(entity.packed());
    return found == by_entity.end() ? nullptr : &nodes[found->second];
}

std::array<Vec2, 4> UiLayout::corners(const UiLayoutNode& node) const {
    return {node.transform.apply({0.0, 0.0}), node.transform.apply({node.size.x, 0.0}),
            node.transform.apply(node.size), node.transform.apply({0.0, node.size.y})};
}

bool UiLayout::contains(const UiLayoutNode& node, const Vec2 point) const {
    if (point.x < node.clip.x0 || point.y < node.clip.y0 || point.x >= node.clip.x1 || point.y >= node.clip.y1)
        return false;
    const auto inverse = node.transform.inverse();
    if (!inverse) return false;
    const auto local = inverse->apply(point);
    return local.x >= 0.0 && local.y >= 0.0 && local.x < node.size.x && local.y < node.size.y;
}

const UiLayoutNode* UiLayout::hit(const Vec2 point, const bool blocking) const {
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const auto& node = nodes[*it];
        if (!node.visible || !node.ui->control) continue;
        const auto filter = node.ui->control->mouse_filter;
        if (filter == UiControl::MouseFilter::ignore) continue;
        if (blocking && filter == UiControl::MouseFilter::pass) continue;
        if (contains(node, point)) return &node;
    }
    return nullptr;
}

void UiImageCache::set_root(const std::filesystem::path& root) {
    if (root == root_) return;
    root_ = root;
    entries_.clear();
}

std::shared_ptr<const UiTexture> UiImageCache::image(const std::string_view path, std::string* error) {
    const auto now = std::chrono::steady_clock::now();
    auto found = entries_.find(path);
    const auto report = [&](const Entry& entry) -> std::shared_ptr<const UiTexture> {
        if (!entry.texture && error) *error = entry.error;
        return entry.texture;
    };
    if (found != entries_.end() && now - found->second.checked < std::chrono::seconds(1)) return report(found->second);
    // Project paths only: no traversal, hidden folders or symbolic links out of the project.
    const auto checked = workspace_file(root_.generic_string(), path, "");
    const auto full_path = checked.value_or(std::filesystem::path{});
    std::error_code code;
    const auto time = std::filesystem::last_write_time(full_path, code);
    const auto size = code ? 0U : std::filesystem::file_size(full_path, code);
    if (found != entries_.end()) {
        found->second.checked = now;
        if (!code && time == found->second.time && size == found->second.size) return report(found->second);
    } else {
        found = entries_.emplace(std::string(path), Entry{}).first;
        found->second.checked = now;
    }
    auto& entry = found->second;
    entry.time = time;
    entry.size = size;
    entry.error.clear();
    // A changed file keeps its texture identity, so renderers replace its pixels in place.
    const auto keep = entry.texture;
    entry.texture.reset();
    if (!checked) {
        entry.error = "not a project file";
        return report(entry);
    }
    if (code || !std::filesystem::is_regular_file(full_path, code)) {
        entry.error = "cannot read the file";
        return report(entry);
    }
    if (size > maximum_file_bytes) {
        entry.error = "larger than 64 MiB";
        return report(entry);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(full_path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    TextureAsset decoded;
    std::string reason;
    if (!stream || !decode_image_rgba(bytes, full_path.extension().string(), decoded, reason)) {
        entry.error = reason.empty() ? "cannot decode the image" : reason;
        return report(entry);
    }
    if (decoded.width == 0U || decoded.height == 0U || decoded.width > maximum_dimension ||
        decoded.height > maximum_dimension) {
        entry.error = "images must be 1 to 8192 pixels on each side";
        return report(entry);
    }
    auto texture = std::make_shared<UiTexture>();
    texture->id = keep ? keep->id : next_ui_texture_id();
    texture->revision = keep ? keep->revision + 1U : 1U;
    texture->width = decoded.width;
    texture->height = decoded.height;
    texture->rgba = std::move(decoded.rgba);
    entry.texture = std::move(texture);
    return entry.texture;
}

void UiPainter::set_root(const std::filesystem::path& root) {
    fonts_.set_root(root);
    images_.set_root(root);
}

void UiPainter::warn(std::string message) {
    if (reported_.size() < 256U && reported_.insert(message).second) warnings_.push_back(std::move(message));
}

std::vector<std::string> UiPainter::take_warnings() { return std::exchange(warnings_, {}); }

// One layout pass: which nodes exist, how they nest, and where each one lands.
struct UiPainter::Build {
    UiPainter& painter;
    const std::vector<UiSourceNode>& sources;
    UiLayout& out;
    std::unordered_map<std::uint64_t, std::size_t> source_of;
    std::unordered_map<std::size_t, Vec2> minimums;

    Build(UiPainter& owner, const std::vector<UiSourceNode>& list, UiLayout& layout)
        : painter(owner), sources(list), out(layout) {}

    void sort(std::vector<std::size_t>& indices) const {
        std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
            const auto order = [&](std::size_t index) {
                const auto& control = out.nodes[index].ui->control;
                return control ? control->order : 0;
            };
            return std::pair{order(a), out.nodes[a].entity.index} < std::pair{order(b), out.nodes[b].entity.index};
        });
    }

    Vec2 label_size(const UiLabel& label) {
        std::string error;
        const auto font = painter.fonts_.font(label.font, label.bold, &error);
        if (!error.empty()) painter.warn(label.font + ": " + error);
        const auto size = static_cast<float>(label.size);
        const auto text = painter.fonts_.layout(font, size, label.text, 0.0F, static_cast<float>(label.line_spacing));
        return {label.wrap ? 0.0 : static_cast<double>(text.width), static_cast<double>(text.height)};
    }

    Vec2 minimum(std::size_t index) {
        if (const auto found = minimums.find(index); found != minimums.end()) return found->second;
        const auto& node = out.nodes[index];
        const auto& ui = *node.ui;
        Vec2 content{};
        const Vec2 text = ui.label ? label_size(*ui.label) : Vec2{};
        if (ui.label) content = text;
        if (ui.button) {
            const auto& padding = ui.button->padding;
            content = {text.x + padding.left + padding.right, text.y + padding.top + padding.bottom};
        }
        if (ui.toggle) {
            const double box = ui.toggle->box_size;
            const double width = ui.toggle->style == UiToggle::Style::switch_ ? box * 1.8 : box;
            content = {width + (ui.label ? ui.toggle->spacing + text.x : 0.0), std::max(box, text.y)};
        }
        if (ui.slider) {
            const double across = std::max(ui.slider->handle_size, ui.slider->track_thickness);
            content = ui.slider->vertical ? Vec2{across, ui.slider->handle_size} : Vec2{ui.slider->handle_size, across};
        }
        if (ui.container) {
            std::vector<Vec2> sizes;
            for (const auto child : node.children)
                if (out.nodes[child].ui->control->visible) sizes.push_back(minimum(child));
            const auto& container = *ui.container;
            const double gaps = sizes.empty() ? 0.0 : container.spacing * static_cast<double>(sizes.size() - 1U);
            Vec2 total{};
            if (container.layout == UiContainer::Layout::grid) {
                const auto columns = static_cast<std::size_t>(std::max(container.columns, 1));
                const auto used = std::min(columns, sizes.size());
                std::vector<double> widths(used, 0.0), heights((sizes.size() + columns - 1U) / columns, 0.0);
                for (std::size_t item = 0; item < sizes.size(); ++item) {
                    widths[item % columns] = std::max(widths[item % columns], sizes[item].x);
                    heights[item / columns] = std::max(heights[item / columns], sizes[item].y);
                }
                for (const double width : widths) total.x += width;
                for (const double height : heights) total.y += height;
                if (used > 1U) total.x += container.spacing * static_cast<double>(used - 1U);
                if (heights.size() > 1U) total.y += container.spacing * static_cast<double>(heights.size() - 1U);
            } else {
                const bool vertical = container.layout == UiContainer::Layout::vertical;
                for (const auto& size : sizes) {
                    if (vertical) {
                        total.x = std::max(total.x, size.x);
                        total.y += size.y;
                    } else {
                        total.x += size.x;
                        total.y = std::max(total.y, size.y);
                    }
                }
                (vertical ? total.y : total.x) += gaps;
            }
            content = {std::max(content.x, total.x + container.padding.left + container.padding.right),
                       std::max(content.y, total.y + container.padding.top + container.padding.bottom)};
        }
        const auto& least = ui.control->min_size;
        const Vec2 result{std::max(least.x, content.x), std::max(least.y, content.y)};
        minimums.emplace(index, result);
        return result;
    }

    // Children of a container, in cells the container computes.
    void arrange(std::size_t index, std::vector<std::pair<Vec2, Vec2>>& rects) {
        const auto& node = out.nodes[index];
        const auto& container = *node.ui->container;
        const auto& padding = container.padding;
        const Vec2 origin{padding.left, padding.top};
        const Vec2 room{std::max(0.0, node.size.x - padding.left - padding.right),
                        std::max(0.0, node.size.y - padding.top - padding.bottom)};
        rects.assign(node.children.size(), {origin, Vec2{}});
        std::vector<std::size_t> shown;
        for (std::size_t item = 0; item < node.children.size(); ++item)
            if (out.nodes[node.children[item]].ui->control->visible) shown.push_back(item);
        if (shown.empty()) return;
        std::vector<Vec2> least;
        for (const auto item : shown) least.push_back(minimum(node.children[item]));
        const auto control_of = [&](std::size_t item) -> const UiControl& {
            return *out.nodes[node.children[item]].ui->control;
        };
        if (container.layout == UiContainer::Layout::grid) {
            const auto columns = static_cast<std::size_t>(std::max(container.columns, 1));
            const auto used = std::min(columns, shown.size());
            const auto rows = (shown.size() + columns - 1U) / columns;
            std::vector<double> widths(used, 0.0), heights(rows, 0.0);
            std::vector<bool> wide(used, false), tall(rows, false);
            for (std::size_t cell = 0; cell < shown.size(); ++cell) {
                const auto column = cell % columns, row = cell / columns;
                widths[column] = std::max(widths[column], least[cell].x);
                heights[row] = std::max(heights[row], least[cell].y);
                if (control_of(shown[cell]).expand_x) wide[column] = true;
                if (control_of(shown[cell]).expand_y) tall[row] = true;
            }
            const auto spread = [&](std::vector<double>& lengths, const std::vector<bool>& expands, double space,
                                    double& lead) {
                double total = container.spacing * static_cast<double>(lengths.size() - 1U);
                for (const double length : lengths) total += length;
                const double extra = std::max(0.0, space - total);
                const auto growing = static_cast<double>(std::count(expands.begin(), expands.end(), true));
                if (growing > 0.0) {
                    for (std::size_t item = 0; item < lengths.size(); ++item)
                        if (expands[item]) lengths[item] += extra / growing;
                    lead = 0.0;
                } else {
                    lead = extra * align_factor(container.align);
                }
            };
            double lead_x = 0.0, lead_y = 0.0;
            spread(widths, wide, room.x, lead_x);
            spread(heights, tall, room.y, lead_y);
            for (std::size_t cell = 0; cell < shown.size(); ++cell) {
                const auto column = cell % columns, row = cell / columns;
                Vec2 at{origin.x + lead_x, origin.y + lead_y};
                for (std::size_t before = 0; before < column; ++before) at.x += widths[before] + container.spacing;
                for (std::size_t before = 0; before < row; ++before) at.y += heights[before] + container.spacing;
                auto& rect = rects[shown[cell]];
                fit_in_cell(control_of(shown[cell]), at, {widths[column], heights[row]}, least[cell], rect.first,
                            rect.second);
            }
            return;
        }
        const bool vertical = container.layout == UiContainer::Layout::vertical;
        const auto along = [vertical](Vec2 value) { return vertical ? value.y : value.x; };
        double total = container.spacing * static_cast<double>(shown.size() - 1U);
        std::size_t growing = 0;
        for (std::size_t item = 0; item < shown.size(); ++item) {
            total += along(least[item]);
            const auto& control = control_of(shown[item]);
            if (vertical ? control.expand_y : control.expand_x) ++growing;
        }
        const double extra = std::max(0.0, along(room) - total);
        double cursor = growing > 0U ? 0.0 : extra * align_factor(container.align);
        for (std::size_t item = 0; item < shown.size(); ++item) {
            const auto& control = control_of(shown[item]);
            double length = along(least[item]);
            if (growing > 0U && (vertical ? control.expand_y : control.expand_x))
                length += extra / static_cast<double>(growing);
            const Vec2 at = vertical ? Vec2{origin.x, origin.y + cursor} : Vec2{origin.x + cursor, origin.y};
            const Vec2 cell = vertical ? Vec2{room.x, length} : Vec2{length, room.y};
            auto& rect = rects[shown[item]];
            fit_in_cell(control, at, cell, least[item], rect.first, rect.second);
            cursor += length + container.spacing;
        }
    }

    void place(std::size_t index, Vec2 position, Vec2 size, const UiAffine& parent, double opacity, bool visible,
               const UiClipRect& clip, std::int32_t layer) {
        auto& node = out.nodes[index];
        const auto& control = *node.ui->control;
        node.position = position;
        node.size = size;
        const Vec2 pivot{control.pivot.x * size.x, control.pivot.y * size.y};
        const auto local = translation(position.x + pivot.x, position.y + pivot.y)
                               .then(rotation(control.rotation))
                               .then(scaling(control.scale.x, control.scale.y))
                               .then(translation(-pivot.x, -pivot.y));
        node.transform = parent.then(local);
        node.parent_transform = parent;
        node.opacity = opacity * control.opacity;
        node.visible = visible && control.visible;
        node.clip = clip;
        node.layer = layer;
        out.order.push_back(index);
        auto inner_clip = clip;
        if (control.clip_contents) {
            const auto corners = out.corners(node);
            UiClipRect bounds{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
            for (const auto& corner : corners) {
                bounds.x0 = std::min(bounds.x0, static_cast<float>(corner.x));
                bounds.y0 = std::min(bounds.y0, static_cast<float>(corner.y));
                bounds.x1 = std::max(bounds.x1, static_cast<float>(corner.x));
                bounds.y1 = std::max(bounds.y1, static_cast<float>(corner.y));
            }
            inner_clip = intersect(clip, bounds);
        }
        const auto children = node.children;
        const auto transform = node.transform;
        const auto shown = node.visible;
        const auto faded = node.opacity;
        std::vector<std::pair<Vec2, Vec2>> rects;
        if (node.ui->container) {
            arrange(index, rects);
        } else {
            for (const auto child : children) rects.push_back(anchored(*out.nodes[child].ui->control, size));
        }
        for (std::size_t item = 0; item < children.size(); ++item)
            place(children[item], rects[item].first, rects[item].second, transform, faded, shown, inner_clip, layer);
    }

    static std::pair<Vec2, Vec2> anchored(const UiControl& control, Vec2 parent) {
        const double left = control.anchor_min.x * parent.x + control.offset_min.x;
        const double top = control.anchor_min.y * parent.y + control.offset_min.y;
        const double right = control.anchor_max.x * parent.x + control.offset_max.x;
        const double bottom = control.anchor_max.y * parent.y + control.offset_max.y;
        return {{left, top},
                {std::max({right - left, control.min_size.x, 0.0}), std::max({bottom - top, control.min_size.y, 0.0})}};
    }

    void run() {
        for (std::size_t index = 0; index < sources.size(); ++index) {
            source_of.emplace(sources[index].entity.packed(), index);
            const auto* ui = sources[index].ui;
            if (!ui || (!ui->canvas && !ui->control)) continue;
            out.by_entity.emplace(sources[index].entity.packed(), out.nodes.size());
            UiLayoutNode node;
            node.entity = sources[index].entity;
            node.ui = ui;
            out.nodes.push_back(std::move(node));
        }
        // Top-level controls belong to the nearest canvas above them, or to the screen.
        std::map<std::size_t, std::vector<std::size_t>> roots; // Canvas node (or none) to controls.
        for (std::size_t index = 0; index < out.nodes.size(); ++index) {
            auto& node = out.nodes[index];
            if (!node.ui->control) continue;
            auto parent = sources[source_of.at(node.entity.packed())].parent;
            if (const auto found = out.by_entity.find(parent.packed());
                parent.valid() && found != out.by_entity.end() && out.nodes[found->second].ui->control) {
                node.parent = found->second;
                out.nodes[found->second].children.push_back(index);
                continue;
            }
            auto canvas = UiLayoutNode::none;
            for (std::size_t depth = 0; parent.valid() && depth < 100000U; ++depth) {
                if (const auto found = out.by_entity.find(parent.packed());
                    found != out.by_entity.end() && out.nodes[found->second].ui->canvas) {
                    canvas = found->second;
                    break;
                }
                const auto source = source_of.find(parent.packed());
                if (source == source_of.end()) break;
                parent = sources[source->second].parent;
            }
            roots[canvas].push_back(index);
        }
        for (auto& node : out.nodes) sort(node.children);
        struct Layer {
            std::size_t canvas;
            std::int32_t order;
            std::uint32_t tiebreak;
        };
        std::vector<Layer> layers;
        for (std::size_t index = 0; index < out.nodes.size(); ++index)
            if (out.nodes[index].ui->canvas)
                layers.push_back({index, out.nodes[index].ui->canvas->sort_order, out.nodes[index].entity.index + 1U});
        // Controls outside any canvas draw on the screen's own layer, below canvases of equal order.
        if (roots.contains(UiLayoutNode::none)) layers.push_back({UiLayoutNode::none, 0, 0U});
        std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) {
            return std::pair{a.order, a.tiebreak} < std::pair{b.order, b.tiebreak};
        });
        const UiClipRect screen{0.0F, 0.0F, static_cast<float>(out.width), static_cast<float>(out.height)};
        for (const auto& layer : layers) {
            double scale = 1.0;
            bool visible = true;
            if (layer.canvas != UiLayoutNode::none) {
                const auto& canvas = *out.nodes[layer.canvas].ui->canvas;
                scale = std::max(canvas_scale(canvas, out.width, out.height), 1e-6);
                visible = canvas.visible;
            }
            const auto transform = scaling(scale, scale);
            const Vec2 size{static_cast<double>(out.width) / scale, static_cast<double>(out.height) / scale};
            if (layer.canvas != UiLayoutNode::none) {
                auto& node = out.nodes[layer.canvas];
                node.size = size;
                node.transform = transform;
                node.parent_transform = transform;
                node.visible = visible;
                node.clip = screen;
                node.layer = layer.order;
                out.order.push_back(layer.canvas);
            }
            auto& members = roots[layer.canvas];
            sort(members);
            for (const auto member : members) {
                const auto [position, extent] = anchored(*out.nodes[member].ui->control, size);
                place(member, position, extent, transform, 1.0, visible, screen, layer.order);
            }
        }
    }
};

UiLayout UiPainter::layout(const std::vector<UiSourceNode>& sources, const std::uint32_t width,
                           const std::uint32_t height) {
    UiLayout result;
    result.width = width;
    result.height = height;
    Build build{*this, sources, result};
    build.run();
    return result;
}

Vec2 UiPainter::minimum_size(const std::vector<UiSourceNode>& sources, const Entity entity) {
    UiLayout result;
    result.width = 1920U;
    result.height = 1080U;
    Build build{*this, sources, result};
    build.run();
    const auto found = result.by_entity.find(entity.packed());
    if (found == result.by_entity.end() || !result.nodes[found->second].ui->control) return {};
    return build.minimum(found->second);
}

namespace {

// Collects triangles into commands, starting a new command whenever the texture or clip changes.
class Mesh {
public:
    Mesh(UiDrawList& list, std::shared_ptr<const UiTexture> white, std::array<float, 2> white_uv)
        : list_(list), white_(std::move(white)), white_uv_(white_uv) {}

    void use(const std::shared_ptr<const UiTexture>& texture, const UiClipRect& clip) {
        const auto found = textures_.find(texture.get());
        if (found == textures_.end()) {
            texture_ = static_cast<std::uint32_t>(list_.textures.size());
            textures_.emplace(texture.get(), texture_);
            list_.textures.push_back(texture);
        } else {
            texture_ = found->second;
        }
        clip_ = clip;
    }
    void solid(const UiClipRect& clip) { use(white_, clip); }
    [[nodiscard]] std::array<float, 2> white_uv() const { return white_uv_; }

    std::uint32_t vertex(Vec2 at, float u, float v, std::uint32_t color) {
        list_.vertices.push_back({static_cast<float>(at.x), static_cast<float>(at.y), u, v, color});
        return static_cast<std::uint32_t>(list_.vertices.size() - 1U);
    }
    std::uint32_t solid_vertex(Vec2 at, std::uint32_t color) { return vertex(at, white_uv_[0], white_uv_[1], color); }
    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (list_.commands.empty() || list_.commands.back().texture != texture_ || list_.commands.back().clip != clip_ ||
            list_.commands.back().first_index + list_.commands.back().index_count != list_.indices.size())
            list_.commands.push_back({texture_, static_cast<std::uint32_t>(list_.indices.size()), 0U, clip_});
        list_.indices.insert(list_.indices.end(), {a, b, c});
        list_.commands.back().index_count += 3U;
    }

private:
    UiDrawList& list_;
    std::shared_ptr<const UiTexture> white_;
    std::array<float, 2> white_uv_;
    std::unordered_map<const UiTexture*, std::uint32_t> textures_;
    std::uint32_t texture_{};
    UiClipRect clip_{};
};

// Points around a rounded rectangle in a control's own space, clockwise on screen, with the same
// count for every radius so rings can join two of them.
std::vector<Vec2> rounded_path(Vec2 low, Vec2 high, double radius, std::size_t segments) {
    if (high.x < low.x) low.x = high.x = (low.x + high.x) * 0.5;
    if (high.y < low.y) low.y = high.y = (low.y + high.y) * 0.5;
    radius = std::clamp(radius, 0.0, std::min(high.x - low.x, high.y - low.y) * 0.5);
    const std::array<Vec2, 4> centers{Vec2{low.x + radius, low.y + radius}, Vec2{high.x - radius, low.y + radius},
                                      Vec2{high.x - radius, high.y - radius}, Vec2{low.x + radius, high.y - radius}};
    std::vector<Vec2> points;
    points.reserve(4U * (segments + 1U));
    for (std::size_t corner = 0; corner < 4U; ++corner) {
        const double start = std::numbers::pi * (1.0 + 0.5 * static_cast<double>(corner));
        for (std::size_t step = 0; step <= segments; ++step) {
            const double angle = start + std::numbers::pi * 0.5 * static_cast<double>(step) / static_cast<double>(segments);
            points.push_back({centers[corner].x + std::cos(angle) * radius, centers[corner].y + std::sin(angle) * radius});
        }
    }
    return points;
}

std::size_t corner_segments(double radius_pixels) {
    if (radius_pixels < 0.75) return 1U;
    const double full = std::numbers::pi / std::acos(std::clamp(1.0 - 0.3 / radius_pixels, -1.0, 1.0));
    return std::clamp(static_cast<std::size_t>(std::ceil(full / 4.0)), std::size_t{2}, std::size_t{32});
}

struct Painter {
    Mesh& mesh;
    UiFontCache& fonts;

    std::vector<std::uint32_t> emit(const UiAffine& transform, const std::vector<Vec2>& path, std::uint32_t color) {
        std::vector<std::uint32_t> indices;
        indices.reserve(path.size());
        for (const auto& point : path) indices.push_back(mesh.solid_vertex(transform.apply(point), color));
        return indices;
    }
    void fan(const std::vector<std::uint32_t>& ring) {
        for (std::size_t index = 1; index + 1U < ring.size(); ++index) mesh.triangle(ring[0], ring[index], ring[index + 1U]);
    }
    void join(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
        for (std::size_t index = 0; index < a.size(); ++index) {
            const auto next = (index + 1U) % a.size();
            mesh.triangle(a[index], a[next], b[next]);
            mesh.triangle(a[index], b[next], b[index]);
        }
    }

    // A rounded rectangle with edges softened over `feather` pixels: one pixel for anti-aliasing,
    // more for shadows.
    void fill_rounded(const UiAffine& transform, Vec2 low, Vec2 high, double radius, const UiColor& color,
                      double opacity, double feather = 1.0) {
        const double scale = transform.scale();
        if (scale <= 0.0 || color.a * opacity <= 0.0 || high.x <= low.x || high.y <= low.y) return;
        const double half = feather * 0.5 / scale;
        const auto segments = corner_segments((radius + half) * scale);
        const auto inner = emit(transform, rounded_path(add(low, {half, half}), subtract(high, {half, half}), radius - half, segments),
                                pack_ui_color(color, opacity));
        const auto outer = emit(transform, rounded_path(subtract(low, {half, half}), add(high, {half, half}), radius + half, segments),
                                pack_ui_color(color, 0.0));
        fan(inner);
        join(inner, outer);
    }

    void stroke_rounded(const UiAffine& transform, Vec2 low, Vec2 high, double radius, double width,
                        const UiColor& color, double opacity) {
        const double scale = transform.scale();
        if (scale <= 0.0 || width <= 0.0 || color.a * opacity <= 0.0) return;
        const double half = 0.5 / scale;
        const auto segments = corner_segments((radius + half) * scale);
        const auto path = [&](double inset, double alpha) {
            return emit(transform,
                        rounded_path(add(low, {inset, inset}), subtract(high, {inset, inset}), radius - inset, segments),
                        pack_ui_color(color, opacity * alpha));
        };
        const double pixels = width * scale;
        if (pixels >= 1.0) {
            const auto outer = path(-half, 0.0), edge = path(half, 1.0), inner_edge = path(width - half, 1.0),
                       inner = path(width + half, 0.0);
            join(outer, edge);
            join(edge, inner_edge);
            join(inner_edge, inner);
        } else {
            const auto outer = path(-half, 0.0), middle = path(width * 0.5, pixels), inner = path(width + half, 0.0);
            join(outer, middle);
            join(middle, inner);
        }
    }

    // A straight stroke, extended by half its width at both ends so joined strokes meet.
    void line(const UiAffine& transform, Vec2 from, Vec2 to, double width, const UiColor& color, double opacity) {
        const double scale = transform.scale();
        const double length = std::hypot(to.x - from.x, to.y - from.y);
        if (scale <= 0.0 || length <= 0.0) return;
        const Vec2 direction{(to.x - from.x) / length, (to.y - from.y) / length};
        const Vec2 normal{-direction.y, direction.x};
        const auto quad = [&](double side, double alpha) {
            const double reach = std::max(side, 0.0);
            const double extend = width * 0.5;
            const Vec2 start{from.x - direction.x * extend, from.y - direction.y * extend};
            const Vec2 end{to.x + direction.x * extend, to.y + direction.y * extend};
            return emit(transform,
                        {Vec2{start.x - normal.x * reach, start.y - normal.y * reach},
                         Vec2{end.x - normal.x * reach, end.y - normal.y * reach},
                         Vec2{end.x + normal.x * reach, end.y + normal.y * reach},
                         Vec2{start.x + normal.x * reach, start.y + normal.y * reach}},
                        pack_ui_color(color, opacity * alpha));
        };
        const double half = 0.5 / scale;
        const auto inner = quad(width * 0.5 - half, 1.0);
        const auto outer = quad(width * 0.5 + half, 0.0);
        fan(inner);
        join(inner, outer);
    }

    // A textured rectangle; UVs map its corners, clockwise from the top-left.
    void image_quad(const UiAffine& transform, Vec2 low, Vec2 high, Vec2 uv_low, Vec2 uv_high, std::uint32_t color) {
        if (high.x <= low.x || high.y <= low.y) return;
        const auto corner = [&](Vec2 at, double u, double v) {
            return mesh.vertex(transform.apply(at), static_cast<float>(u), static_cast<float>(v), color);
        };
        const auto a = corner(low, uv_low.x, uv_low.y);
        const auto b = corner({high.x, low.y}, uv_high.x, uv_low.y);
        const auto c = corner(high, uv_high.x, uv_high.y);
        const auto d = corner({low.x, high.y}, uv_low.x, uv_high.y);
        mesh.triangle(a, b, c);
        mesh.triangle(a, c, d);
    }
};

} // namespace

UiDrawList UiPainter::draw(const UiLayout& layout, const UiVisualState& state) {
    UiDrawList list;
    list.width = layout.width;
    list.height = layout.height;
    if (layout.width == 0U || layout.height == 0U) return list;
    fonts_.trim();
    Mesh mesh{list, fonts_.pages().front(), fonts_.white_uv()};
    Painter paint{mesh, fonts_};
    for (const auto index : layout.order) {
        const auto& node = layout.nodes[index];
        if (!node.visible || !node.ui->control || node.opacity <= 0.0) continue;
        const auto& ui = *node.ui;
        const auto& transform = node.transform;
        const Vec2 size = node.size;
        const bool hovered = state.hovered == node.entity;
        const bool held = state.pressed == node.entity;
        const bool disabled = (ui.button && ui.button->disabled) || (ui.toggle && ui.toggle->disabled) ||
                              (ui.slider && ui.slider->disabled);
        const double opacity = node.opacity;
        mesh.solid(node.clip);

        // A button's state replaces its panel's color, or tints its image when it has no panel.
        std::optional<UiColor> state_color;
        if (ui.button) {
            const auto& button = *ui.button;
            if (button.disabled) state_color = button.disabled_color;
            else if ((held && hovered) || (button.toggle && button.pressed)) state_color = button.pressed_color;
            else if (hovered || held) state_color = button.hover_color;
        }
        if (ui.panel) {
            const auto& panel = *ui.panel;
            if (panel.shadow_color.a > 0.0 && panel.shadow_size > 0.0) {
                const Vec2 shift = panel.shadow_offset;
                paint.fill_rounded(transform, shift, add(size, shift), panel.corner_radius, panel.shadow_color, opacity,
                                   panel.shadow_size * transform.scale());
            }
            paint.fill_rounded(transform, {}, size, panel.corner_radius, state_color.value_or(panel.color), opacity);
            paint.stroke_rounded(transform, {}, size, panel.corner_radius, panel.border_width, panel.border_color, opacity);
        }
        if (ui.image && !ui.image->image.empty()) {
            const auto& image = *ui.image;
            std::string error;
            const auto texture = images_.image(image.image, &error);
            if (!texture) {
                warn(image.image + ": " + error);
            } else {
                auto tint = image.color;
                if (state_color && !ui.panel) tint = multiply(tint, *state_color);
                const auto color = pack_ui_color(tint, opacity);
                mesh.use(texture, node.clip);
                const double width = texture->width, height = texture->height;
                // Flipping mirrors the texture coordinates of whatever the mode draws.
                const auto u = [&](double value) { return image.flip_x ? 1.0 - value : value; };
                const auto v = [&](double value) { return image.flip_y ? 1.0 - value : value; };
                const auto quad = [&](Vec2 low, Vec2 high, Vec2 uv_low, Vec2 uv_high) {
                    paint.image_quad(transform, low, high, {u(uv_low.x), v(uv_low.y)}, {u(uv_high.x), v(uv_high.y)}, color);
                };
                switch (image.mode) {
                case UiImage::Mode::stretch: quad({}, size, {0, 0}, {1, 1}); break;
                case UiImage::Mode::fit:
                case UiImage::Mode::center: {
                    const double scale = image.mode == UiImage::Mode::center
                                             ? 1.0
                                             : std::min(size.x / width, size.y / height);
                    const Vec2 drawn{width * scale, height * scale};
                    const Vec2 low{(size.x - drawn.x) * 0.5, (size.y - drawn.y) * 0.5};
                    quad(low, add(low, drawn), {0, 0}, {1, 1});
                    break;
                }
                case UiImage::Mode::fill: {
                    const double scale = std::max(size.x / width, size.y / height);
                    const Vec2 shown{size.x / (width * scale), size.y / (height * scale)};
                    const Vec2 low{(1.0 - shown.x) * 0.5, (1.0 - shown.y) * 0.5};
                    quad({}, size, low, add(low, shown));
                    break;
                }
                case UiImage::Mode::tile: {
                    const auto columns = static_cast<std::size_t>(std::ceil(size.x / width));
                    const auto rows = static_cast<std::size_t>(std::ceil(size.y / height));
                    if (columns * rows > 4096U) {
                        quad({}, size, {0, 0}, {1, 1});
                        break;
                    }
                    for (std::size_t row = 0; row < rows; ++row)
                        for (std::size_t column = 0; column < columns; ++column) {
                            const Vec2 low{static_cast<double>(column) * width, static_cast<double>(row) * height};
                            const Vec2 high{std::min(low.x + width, size.x), std::min(low.y + height, size.y)};
                            quad(low, high, {0, 0}, {(high.x - low.x) / width, (high.y - low.y) / height});
                        }
                    break;
                }
                case UiImage::Mode::sliced: {
                    const auto& slice = image.slice;
                    // Kept edges shrink together when the control is smaller than they are.
                    double left = slice.left * image.slice_scale, right = slice.right * image.slice_scale;
                    double top = slice.top * image.slice_scale, bottom = slice.bottom * image.slice_scale;
                    if (left + right > size.x && left + right > 0.0) {
                        const double fit = size.x / (left + right);
                        left *= fit;
                        right *= fit;
                    }
                    if (top + bottom > size.y && top + bottom > 0.0) {
                        const double fit = size.y / (top + bottom);
                        top *= fit;
                        bottom *= fit;
                    }
                    const std::array<double, 4> xs{0.0, left, size.x - right, size.x};
                    const std::array<double, 4> ys{0.0, top, size.y - bottom, size.y};
                    const std::array<double, 4> us{0.0, slice.left / width, 1.0 - slice.right / width, 1.0};
                    const std::array<double, 4> vs{0.0, slice.top / height, 1.0 - slice.bottom / height, 1.0};
                    for (std::size_t row = 0; row < 3U; ++row)
                        for (std::size_t column = 0; column < 3U; ++column)
                            quad({xs[column], ys[row]}, {xs[column + 1U], ys[row + 1U]}, {us[column], vs[row]},
                                 {us[column + 1U], vs[row + 1U]});
                    break;
                }
                }
                mesh.solid(node.clip);
            }
        }
        if (ui.progress_bar) {
            const auto& bar = *ui.progress_bar;
            const double amount = std::clamp((bar.value - bar.minimum) / (bar.maximum - bar.minimum), 0.0, 1.0);
            const double inset = bar.fill_inset;
            Vec2 low{inset, inset}, high{size.x - inset, size.y - inset};
            const double radius = ui.panel ? std::max(0.0, ui.panel->corner_radius - inset) : 0.0;
            switch (bar.direction) {
            case UiProgressBar::Direction::left_to_right: high.x = low.x + (high.x - low.x) * amount; break;
            case UiProgressBar::Direction::right_to_left: low.x = high.x - (high.x - low.x) * amount; break;
            case UiProgressBar::Direction::top_to_bottom: high.y = low.y + (high.y - low.y) * amount; break;
            case UiProgressBar::Direction::bottom_to_top: low.y = high.y - (high.y - low.y) * amount; break;
            }
            if (amount > 0.0) paint.fill_rounded(transform, low, high, radius, bar.fill_color, opacity);
        }
        if (ui.slider) {
            const auto& slider = *ui.slider;
            const double faded = opacity * (slider.disabled ? 0.5 : 1.0);
            const double amount = std::clamp((slider.value - slider.minimum) / (slider.maximum - slider.minimum), 0.0, 1.0);
            const double handle = slider.handle_size * (held ? 0.92 : hovered ? 1.08 : 1.0);
            const double thickness = slider.track_thickness;
            if (!slider.vertical) {
                const double start = slider.handle_size * 0.5, end = std::max(start, size.x - slider.handle_size * 0.5);
                const double middle = size.y * 0.5, at = start + (end - start) * amount;
                const Vec2 low{start - thickness * 0.5, middle - thickness * 0.5};
                paint.fill_rounded(transform, low, {end + thickness * 0.5, middle + thickness * 0.5}, thickness * 0.5,
                                   slider.track_color, faded);
                paint.fill_rounded(transform, low, {at, middle + thickness * 0.5}, thickness * 0.5, slider.fill_color, faded);
                paint.fill_rounded(transform, {at - handle * 0.5, middle - handle * 0.5},
                                   {at + handle * 0.5, middle + handle * 0.5}, handle * 0.5, slider.handle_color, faded);
            } else {
                const double start = size.y - slider.handle_size * 0.5;
                const double end = std::min(start, slider.handle_size * 0.5);
                const double middle = size.x * 0.5, at = start + (end - start) * amount;
                paint.fill_rounded(transform, {middle - thickness * 0.5, end - thickness * 0.5},
                                   {middle + thickness * 0.5, start + thickness * 0.5}, thickness * 0.5,
                                   slider.track_color, faded);
                paint.fill_rounded(transform, {middle - thickness * 0.5, at}, {middle + thickness * 0.5, start + thickness * 0.5},
                                   thickness * 0.5, slider.fill_color, faded);
                paint.fill_rounded(transform, {middle - handle * 0.5, at - handle * 0.5},
                                   {middle + handle * 0.5, at + handle * 0.5}, handle * 0.5, slider.handle_color, faded);
            }
        }
        double text_left = 0.0;
        if (ui.toggle) {
            const auto& toggle = *ui.toggle;
            const double faded = opacity * (toggle.disabled ? 0.5 : 1.0);
            const double box = toggle.box_size;
            const double top = (size.y - box) * 0.5;
            const auto light = [&](const UiColor& color) {
                return hovered && !toggle.disabled ? mix(color, {1.0, 1.0, 1.0, color.a}, 0.12) : color;
            };
            if (toggle.style == UiToggle::Style::switch_) {
                const double width = box * 1.8;
                paint.fill_rounded(transform, {0.0, top}, {width, top + box}, box * 0.5,
                                   light(toggle.checked ? toggle.check_color : toggle.box_color), faded);
                const double knob = box * 0.76, gap = (box - knob) * 0.5;
                const double left = toggle.checked ? width - gap - knob : gap;
                paint.fill_rounded(transform, {left, top + gap}, {left + knob, top + gap + knob}, knob * 0.5,
                                   {0.97, 0.97, 0.98, 1.0}, faded);
                text_left = width + toggle.spacing;
            } else {
                paint.fill_rounded(transform, {0.0, top}, {box, top + box}, box * 0.22,
                                   light(toggle.checked ? toggle.check_color : toggle.box_color), faded);
                if (toggle.checked) {
                    const UiColor mark{1.0, 1.0, 1.0, 1.0};
                    const double stroke = box * 0.12;
                    paint.line(transform, {box * 0.26, top + box * 0.53}, {box * 0.43, top + box * 0.70}, stroke, mark, faded);
                    paint.line(transform, {box * 0.43, top + box * 0.70}, {box * 0.75, top + box * 0.33}, stroke, mark, faded);
                }
                text_left = box + toggle.spacing;
            }
        }
        if (ui.label && !ui.label->text.empty()) {
            const auto& label = *ui.label;
            Vec2 low{text_left, 0.0}, high = size;
            if (ui.button) {
                low = add(low, {ui.button->padding.left, ui.button->padding.top});
                high = subtract(high, {ui.button->padding.right, ui.button->padding.bottom});
            }
            const double scale = transform.scale();
            const float pixels = static_cast<float>(std::max(1.0, std::round(label.size * scale)));
            if (scale > 0.0 && label.size * scale >= 0.5) {
                const double text_scale = pixels / label.size; // Pixels per unit at the drawn size.
                std::string error;
                const auto font = fonts_.font(label.font, label.bold, &error);
                if (!error.empty()) warn(label.font + ": " + error);
                const double box_width = std::max(0.0, high.x - low.x) * text_scale;
                const double box_height = std::max(0.0, high.y - low.y) * text_scale;
                const auto text = fonts_.layout(font, pixels, label.text,
                                                label.wrap ? static_cast<float>(std::max(box_width, 1.0)) : 0.0F,
                                                static_cast<float>(label.line_spacing));
                const double top = (box_height - text.height) * text_factor(label.vertical_align);
                // Unrotated text snaps to whole pixels, where glyph bitmaps are sharpest.
                const bool upright = std::abs(transform.b) < 1e-9 && std::abs(transform.c) < 1e-9 && transform.a > 0.0 &&
                                     transform.d > 0.0;
                const auto origin = transform.apply(low);
                const double faded = opacity * (disabled ? 0.5 : 1.0);
                const auto pass = [&](Vec2 shift, const UiColor& color) {
                    const auto packed = pack_ui_color(color, faded);
                    for (const auto& placed : text.glyphs) {
                        auto owner = placed.font;
                        const auto glyph = fonts_.glyph(owner, pixels, placed.codepoint);
                        if (!glyph.visible) continue;
                        mesh.use(fonts_.pages()[glyph.page], node.clip);
                        const double pen_x = (box_width - text.line_widths[placed.line]) * text_factor(label.horizontal_align) +
                                             placed.x + shift.x;
                        const double pen_y = top + text.ascent + text.line_height * static_cast<double>(placed.line) + shift.y;
                        std::array<Vec2, 4> corners;
                        if (upright) {
                            const Vec2 pen{std::round(origin.x + pen_x * transform.a / text_scale),
                                           std::round(origin.y + pen_y * transform.d / text_scale)};
                            corners = {Vec2{pen.x + glyph.x0, pen.y + glyph.y0}, Vec2{pen.x + glyph.x1, pen.y + glyph.y0},
                                       Vec2{pen.x + glyph.x1, pen.y + glyph.y1}, Vec2{pen.x + glyph.x0, pen.y + glyph.y1}};
                        } else {
                            const auto at = [&](double x, double y) {
                                return transform.apply({low.x + (pen_x + x) / text_scale, low.y + (pen_y + y) / text_scale});
                            };
                            corners = {at(glyph.x0, glyph.y0), at(glyph.x1, glyph.y0), at(glyph.x1, glyph.y1),
                                       at(glyph.x0, glyph.y1)};
                        }
                        const auto a = mesh.vertex(corners[0], glyph.u0, glyph.v0, packed);
                        const auto b = mesh.vertex(corners[1], glyph.u1, glyph.v0, packed);
                        const auto c = mesh.vertex(corners[2], glyph.u1, glyph.v1, packed);
                        const auto d = mesh.vertex(corners[3], glyph.u0, glyph.v1, packed);
                        mesh.triangle(a, b, c);
                        mesh.triangle(a, c, d);
                    }
                };
                if (label.shadow_color.a > 0.0)
                    pass({label.shadow_offset.x * text_scale, label.shadow_offset.y * text_scale}, label.shadow_color);
                if (label.outline_size > 0.0 && label.outline_color.a > 0.0) {
                    const double reach = label.outline_size * text_scale;
                    const int steps = reach > 2.0 ? 16 : 8;
                    for (int step = 0; step < steps; ++step) {
                        const double angle = 2.0 * std::numbers::pi * step / steps;
                        pass({std::cos(angle) * reach, std::sin(angle) * reach}, label.outline_color);
                    }
                }
                pass({}, label.color);
                mesh.solid(node.clip);
            }
        }
    }
    return list;
}

namespace {

double srgb_to_linear(double value) {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

const std::array<float, 256>& decode_table() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> values{};
        for (std::size_t index = 0; index < values.size(); ++index)
            values[index] = static_cast<float>(srgb_to_linear(static_cast<double>(index) / 255.0));
        return values;
    }();
    return table;
}

std::array<float, 4> sample(const UiTexture& texture, float u, float v) {
    const float x = std::clamp(u * static_cast<float>(texture.width) - 0.5F, 0.0F,
                               static_cast<float>(texture.width) - 1.0F);
    const float y = std::clamp(v * static_cast<float>(texture.height) - 0.5F, 0.0F,
                               static_cast<float>(texture.height) - 1.0F);
    const auto x0 = static_cast<std::uint32_t>(x), y0 = static_cast<std::uint32_t>(y);
    const auto x1 = std::min(x0 + 1U, texture.width - 1U), y1 = std::min(y0 + 1U, texture.height - 1U);
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    std::array<float, 4> result{};
    for (std::size_t channel = 0; channel < 4U; ++channel) {
        const auto at = [&](std::uint32_t px, std::uint32_t py) {
            return static_cast<float>(texture.rgba[(static_cast<std::size_t>(py) * texture.width + px) * 4U + channel]) / 255.0F;
        };
        const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
        const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
        result[channel] = top + (bottom - top) * fy;
    }
    return result;
}

} // namespace

void rasterize_ui(const UiDrawList& list, std::vector<std::uint8_t>& rgba) {
    const auto width = static_cast<std::int64_t>(list.width), height = static_cast<std::int64_t>(list.height);
    if (rgba.size() != static_cast<std::size_t>(width * height) * 4U) return;
    const auto& decode = decode_table();
    const auto unpack = [](std::uint32_t color, std::size_t channel) {
        return static_cast<float>((color >> (8U * channel)) & 0xFFU) / 255.0F;
    };
    for (const auto& command : list.commands) {
        if (command.texture >= list.textures.size()) continue;
        const auto& texture = *list.textures[command.texture];
        const auto clip_x0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(command.clip.x0)));
        const auto clip_y0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(command.clip.y0)));
        const auto clip_x1 = std::min<std::int64_t>(width, static_cast<std::int64_t>(std::ceil(command.clip.x1)));
        const auto clip_y1 = std::min<std::int64_t>(height, static_cast<std::int64_t>(std::ceil(command.clip.y1)));
        for (std::uint32_t item = 0; item + 2U < command.index_count; item += 3U) {
            const auto& a = list.vertices[list.indices[command.first_index + item]];
            const auto& b = list.vertices[list.indices[command.first_index + item + 1U]];
            const auto& c = list.vertices[list.indices[command.first_index + item + 2U]];
            const double area = (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) -
                                (static_cast<double>(b.y) - a.y) * (static_cast<double>(c.x) - a.x);
            if (std::abs(area) < 1e-12) continue;
            const auto x0 = std::max(clip_x0, static_cast<std::int64_t>(std::floor(std::min({a.x, b.x, c.x}))));
            const auto y0 = std::max(clip_y0, static_cast<std::int64_t>(std::floor(std::min({a.y, b.y, c.y}))));
            const auto x1 = std::min(clip_x1, static_cast<std::int64_t>(std::ceil(std::max({a.x, b.x, c.x}))) + 1);
            const auto y1 = std::min(clip_y1, static_cast<std::int64_t>(std::ceil(std::max({a.y, b.y, c.y}))) + 1);
            // Edge functions oriented so inside is positive, with a top-left rule for shared edges.
            const double sign = area > 0.0 ? 1.0 : -1.0;
            const auto edge = [&](const UiVertex& p, const UiVertex& q, double x, double y) {
                return sign * ((static_cast<double>(q.x) - p.x) * (y - p.y) - (static_cast<double>(q.y) - p.y) * (x - p.x));
            };
            const auto owns = [&](const UiVertex& p, const UiVertex& q) {
                const double dx = sign * (static_cast<double>(q.x) - p.x), dy = sign * (static_cast<double>(q.y) - p.y);
                return (dy == 0.0 && dx > 0.0) || dy < 0.0;
            };
            const bool own_ab = owns(a, b), own_bc = owns(b, c), own_ca = owns(c, a);
            for (auto y = y0; y < y1; ++y)
                for (auto x = x0; x < x1; ++x) {
                    const double px = static_cast<double>(x) + 0.5, py = static_cast<double>(y) + 0.5;
                    const double w_c = edge(a, b, px, py), w_a = edge(b, c, px, py), w_b = edge(c, a, px, py);
                    if (w_c < 0.0 || w_a < 0.0 || w_b < 0.0) continue;
                    if ((w_c == 0.0 && !own_ab) || (w_a == 0.0 && !own_bc) || (w_b == 0.0 && !own_ca)) continue;
                    const double total = w_a + w_b + w_c;
                    const auto la = static_cast<float>(w_a / total), lb = static_cast<float>(w_b / total),
                               lc = static_cast<float>(w_c / total);
                    const auto texel = sample(texture, a.u * la + b.u * lb + c.u * lc, a.v * la + b.v * lb + c.v * lc);
                    std::array<float, 4> color{};
                    for (std::size_t channel = 0; channel < 4U; ++channel)
                        color[channel] = (unpack(a.color, channel) * la + unpack(b.color, channel) * lb +
                                          unpack(c.color, channel) * lc) *
                                         texel[channel];
                    const float alpha = std::clamp(color[3], 0.0F, 1.0F);
                    if (alpha <= 0.0F) continue;
                    auto* pixel = &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                         static_cast<std::size_t>(x)) * 4U];
                    for (std::size_t channel = 0; channel < 3U; ++channel) {
                        const double source = srgb_to_linear(std::clamp(static_cast<double>(color[channel]), 0.0, 1.0));
                        const double destination = decode[pixel[channel]];
                        pixel[channel] = to_byte(linear_to_srgb(source * alpha + destination * (1.0 - alpha)));
                    }
                    pixel[3] = to_byte(alpha + static_cast<double>(pixel[3]) / 255.0 * (1.0 - alpha));
                }
        }
    }
}

} // namespace relay
