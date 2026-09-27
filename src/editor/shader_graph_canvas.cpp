#include "relay/editor/shader_graph_canvas.hpp"

#include "relay/editor/editor_theme.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>

namespace relay {
namespace {

// Node geometry in canvas units (scaled by zoom and the interface scale on screen).
constexpr float node_width = 180.0F;
constexpr float output_width = 280.0F;
constexpr float header_height = 24.0F;
constexpr float row_height = 22.0F;
constexpr float pin_radius = 5.0F;

std::string binary_title(const std::string& op) {
    static const std::map<std::string, std::string> names{
        {"+", "Add"},          {"-", "Subtract"},        {"*", "Multiply"},      {"/", "Divide"},
        {"%", "Remainder"},    {"<", "Less than"},       {">", "Greater than"},  {"<=", "Less or equal"},
        {">=", "Greater or equal"}, {"==", "Equal"},      {"!=", "Not equal"},    {"&&", "And"},
        {"||", "Or"},          {"^^", "Either"}};
    const auto found = names.find(op);
    return found == names.end() ? op : found->second;
}

bool valid_components(const std::string& text) {
    if (text.empty() || text.size() > 4U) return false;
    for (const auto set : {std::string_view{"xyzw"}, std::string_view{"rgba"}, std::string_view{"stpq"}})
        if (std::all_of(text.begin(), text.end(), [&](char c) { return set.find(c) != std::string_view::npos; }))
            return true;
    return false;
}

std::size_t header_rows(const GraphNode& node) {
    // Swizzle and set nodes show their components in a row of their own.
    return node.kind == GraphNode::Kind::swizzle || node.kind == GraphNode::Kind::set ? 1U : 0U;
}

float node_height(const GraphNode& node) {
    const auto rows = header_rows(node) + node.inputs.size();
    return header_height + static_cast<float>(std::max<std::size_t>(rows, 1U)) * row_height + 6.0F;
}

float width_of(const GraphNode& node) {
    return node.kind == GraphNode::Kind::output ? output_width : node_width;
}

ImU32 header_color(const GraphNode& node) {
    switch (node.kind) {
    case GraphNode::Kind::input: return IM_COL32(40, 110, 120, 255);
    case GraphNode::Kind::parameter: return IM_COL32(100, 70, 140, 255);
    case GraphNode::Kind::output: return IM_COL32(50, 120, 70, 255);
    case GraphNode::Kind::swizzle:
    case GraphNode::Kind::set: return IM_COL32(140, 95, 40, 255);
    case GraphNode::Kind::call:
        if (node.op == "vec2" || node.op == "vec3" || node.op == "vec4") return IM_COL32(140, 95, 40, 255);
        if (node.op == "texture" || node.op == "textureLod") return IM_COL32(150, 60, 60, 255);
        return IM_COL32(60, 80, 120, 255);
    default: return IM_COL32(60, 80, 120, 255);
    }
}

float distance_squared(const ImVec2 a, const ImVec2 b) {
    const float x = a.x - b.x, y = a.y - b.y;
    return x * x + y * y;
}

} // namespace

std::string shader_graph_node_title(const ShaderGraph& graph, const GraphNode& node) {
    switch (node.kind) {
    case GraphNode::Kind::input: return node.op;
    case GraphNode::Kind::parameter: return node.op;
    case GraphNode::Kind::binary: return binary_title(node.op);
    case GraphNode::Kind::unary: return node.op == "!" ? "Not" : "Negate";
    case GraphNode::Kind::select: return "Select";
    case GraphNode::Kind::swizzle: return "Split";
    case GraphNode::Kind::set: return "Set components";
    case GraphNode::Kind::call:
        if (node.op == "vec2" || node.op == "vec3" || node.op == "vec4") return "Combine " + node.op;
        return node.op;
    case GraphNode::Kind::output:
        return graph.type == ShaderType::post_process ? "Post-processing output"
               : node.op == "vertex"                   ? "Vertex output"
                                                       : "Surface output";
    }
    return node.op;
}

std::string shader_graph_pin_label(const GraphNode& node, const std::size_t pin) {
    if (pin >= node.inputs.size()) return {};
    switch (node.kind) {
    case GraphNode::Kind::select: {
        static constexpr std::array<const char*, 3> labels{"if", "then", "else"};
        return pin < labels.size() ? labels[pin] : node.inputs[pin].name;
    }
    case GraphNode::Kind::call:
        if (const auto* function = find_graph_function(node.op, node.inputs.size());
            function && pin < function->parameters.size() && function->parameters.size() == node.inputs.size())
            return std::string{function->parameters[pin]};
        return node.inputs[pin].name;
    default: return node.inputs[pin].name;
    }
}

std::vector<ShaderGraphEntry> shader_graph_entries(const ShaderGraph& graph, const GraphStage& stage) {
    std::vector<ShaderGraphEntry> entries;
    for (const auto& builtin : graph_stage_inputs(graph.type, stage.name))
        entries.push_back({std::string{builtin.name}, "Inputs", std::string{builtin.description},
                           GraphNode::Kind::input, std::string{builtin.name}, 0U, {}});
    for (const auto& uniform : graph.uniforms)
        entries.push_back({uniform.name, "Parameters",
                           "Material parameter (" + std::string{shader_uniform_type_name(uniform.type)} + ')',
                           GraphNode::Kind::parameter, uniform.name, 0U, {}});
    for (const auto& [op, one] : std::initializer_list<std::pair<const char*, const char*>>{
             {"+", "0.0"}, {"-", "0.0"}, {"*", "1.0"}, {"/", "1.0"}})
        entries.push_back({binary_title(op), "Math", std::string{"a "} + op + " b", GraphNode::Kind::binary, op, 2U, one});
    entries.push_back({"Negate", "Math", "-a", GraphNode::Kind::unary, "-", 1U, {}});
    for (const auto* op : {"<", ">", "<=", ">=", "==", "!=", "&&", "||"})
        entries.push_back({binary_title(op), "Logic", std::string{"a "} + op + " b", GraphNode::Kind::binary, op, 2U, "0.0"});
    entries.push_back({"Not", "Logic", "!a", GraphNode::Kind::unary, "!", 1U, {}});
    entries.push_back({"Select", "Logic", "if ? then : else", GraphNode::Kind::select, "?", 3U, {}});
    entries.push_back({"Split", "Vector", "Read components, such as x, rgb or zy", GraphNode::Kind::swizzle, "x", 1U, {}});
    entries.push_back({"Set components", "Vector", "Replace components of a vector", GraphNode::Kind::set, "x", 2U, {}});
    for (const auto& function : graph_functions()) {
        if (function.category == "Scene" && graph.type != ShaderType::post_process) continue;
        if (function.category == "Derivative" && stage.name == "vertex") continue;
        const bool constructor = function.name == "vec2" || function.name == "vec3" || function.name == "vec4";
        entries.push_back({constructor ? "Combine " + std::string{function.name} : std::string{function.name},
                           std::string{function.category}, std::string{function.description},
                           GraphNode::Kind::call, std::string{function.name}, function.parameters.size(), {}});
    }
    // The shader's own functions, found by their definitions.
    const auto parsed = parse_relay_shader("shader_type surface;\n" + graph.functions);
    for (const auto& function : parsed.functions) {
        const auto head = std::string_view{parsed.code}.substr(function.begin, function.end - function.begin);
        const auto open = head.find('('), close = head.find(')');
        if (open == std::string_view::npos || close == std::string_view::npos) continue;
        const auto parameters = head.substr(open + 1U, close - open - 1U);
        std::size_t count = parameters.find_first_not_of(" \t\n") == std::string_view::npos ||
                                    parameters.find_first_not_of(" \t\n") == parameters.find("void")
                                ? 0U
                                : static_cast<std::size_t>(std::count(parameters.begin(), parameters.end(), ',')) + 1U;
        entries.push_back({function.name, "Shader functions", "Defined in this shader's functions",
                           GraphNode::Kind::call, function.name, count, {}});
    }
    return entries;
}

GraphClipboard& shader_graph_clipboard() {
    static GraphClipboard clipboard;
    return clipboard;
}

std::string add_shader_graph_node(ShaderGraph& graph, GraphStage& stage, const ShaderGraphEntry& entry, const ImVec2 at) {
    std::string id;
    if (entry.kind == GraphNode::Kind::input) id = "input:" + entry.op;
    if (entry.kind == GraphNode::Kind::parameter) id = "uniform:" + entry.op;
    if (!id.empty()) {
        if (auto* existing = stage.find(id)) {
            existing->x = at.x;
            existing->y = at.y;
            return id;
        }
    } else {
        id = unused_graph_id(stage);
    }
    GraphNode node;
    node.id = id;
    node.kind = entry.kind;
    node.op = entry.op;
    node.x = at.x;
    node.y = at.y;
    switch (entry.kind) {
    case GraphNode::Kind::binary: node.inputs = {{"a", {}, entry.default_value}, {"b", {}, entry.default_value}}; break;
    case GraphNode::Kind::unary: node.inputs = {{"a", {}, "0.0"}}; break;
    case GraphNode::Kind::select: node.inputs = {{"condition", {}, "true"}, {"a", {}, "0.0"}, {"b", {}, "0.0"}}; break;
    case GraphNode::Kind::swizzle: node.inputs = {{"vector", {}, {}}}; break;
    case GraphNode::Kind::set: node.inputs = {{"vector", {}, {}}, {"value", {}, "0.0"}}; break;
    case GraphNode::Kind::call:
        for (std::size_t index = 0; index < entry.inputs; ++index)
            node.inputs.push_back({std::to_string(index + 1U), {}, {}});
        break;
    default: break;
    }
    stage.nodes.push_back(std::move(node));
    (void)update_graph_types(graph, stage);
    return id;
}

ShaderGraphCanvasResult draw_shader_graph_canvas(ShaderGraph& graph, GraphStage& stage, ShaderGraphView& view,
                                                 const std::map<std::string, std::string>& node_errors,
                                                 const ShaderGraphNote& note, const float ui_scale) {
    ShaderGraphCanvasResult result;
    const auto& palette = editor_palette();
    auto& io = ImGui::GetIO();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(ImGui::GetContentRegionAvail().x, 50.0F), std::max(ImGui::GetContentRegionAvail().y, 50.0F)};
    const float scale = view.zoom * ui_scale;
    const auto to_screen = [&](const ImVec2 point) {
        return ImVec2{origin.x + view.pan.x + point.x * scale, origin.y + view.pan.y + point.y * scale};
    };
    const auto to_canvas = [&](const ImVec2 point) {
        return ImVec2{(point.x - origin.x - view.pan.x) / scale, (point.y - origin.y - view.pan.y) / scale};
    };
    // Frame the graph the first time it is shown.
    if (view.frame_pending && !stage.nodes.empty()) {
        view.frame_pending = false;
        float left = 1e9F, top = 1e9F, right = -1e9F, bottom = -1e9F;
        for (const auto& node : stage.nodes) {
            left = std::min(left, node.x);
            top = std::min(top, node.y);
            right = std::max(right, node.x + width_of(node));
            bottom = std::max(bottom, node.y + node_height(node));
        }
        const float fit = std::min(size.x / ((right - left + 80.0F) * ui_scale), size.y / ((bottom - top + 80.0F) * ui_scale));
        // Readable first: a graph wider than the view is reached by panning, not shrunk away.
        view.zoom = std::clamp(fit, 0.6F, 1.0F);
        const float fitted = view.zoom * ui_scale;
        view.pan = {40.0F * fitted - left * fitted, 40.0F * fitted - top * fitted};
    }

    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##graph_canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const auto canvas_id = ImGui::GetItemID();
    if (note) note("shader_graph:canvas", origin, {origin.x + size.x, origin.y + size.y});
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, palette.window);
    // A grid that moves with the canvas.
    const float step = 32.0F * scale;
    if (step > 6.0F) {
        for (float x = std::fmod(view.pan.x, step); x < size.x; x += step)
            draw->AddLine({origin.x + x, origin.y}, {origin.x + x, origin.y + size.y}, IM_COL32(255, 255, 255, 10));
        for (float y = std::fmod(view.pan.y, step); y < size.y; y += step)
            draw->AddLine({origin.x, origin.y + y}, {origin.x + size.x, origin.y + y}, IM_COL32(255, 255, 255, 10));
    }

    // Pin positions on screen.
    const auto output_pin = [&](const GraphNode& node) {
        return to_screen({node.x + width_of(node), node.y + header_height * 0.5F});
    };
    const auto input_pin = [&](const GraphNode& node, const std::size_t pin) {
        return to_screen({node.x, node.y + header_height +
                                      (static_cast<float>(header_rows(node) + pin) + 0.5F) * row_height});
    };
    const auto wire = [&](const ImVec2 from, const ImVec2 to, const ImU32 color) {
        const float bend = std::max(40.0F * scale, std::abs(to.x - from.x) * 0.5F);
        draw->AddBezierCubic(from, {from.x + bend, from.y}, {to.x - bend, to.y}, to, color, 2.0F * ui_scale);
    };

    // Wires first, under the nodes.
    for (const auto& node : stage.nodes)
        for (std::size_t pin = 0; pin < node.inputs.size(); ++pin) {
            const auto* source = node.inputs[pin].source.empty() ? nullptr : stage.find(node.inputs[pin].source);
            if (!source) continue;
            const bool lit = view.selected.contains(node.id) || view.selected.contains(source->id);
            wire(output_pin(*source), input_pin(node, pin), lit ? palette.accent : IM_COL32(170, 170, 180, 200));
        }

    // Draw (and hit-test) nodes in the view's order, most recently clicked last.
    std::vector<GraphNode*> ordered;
    for (auto& node : stage.nodes) ordered.push_back(&node);
    const auto rank = [&](const std::string& id) {
        const auto found = std::find(view.order.begin(), view.order.end(), id);
        return found == view.order.end() ? -1 : static_cast<int>(found - view.order.begin());
    };
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](const GraphNode* a, const GraphNode* b) { return rank(a->id) < rank(b->id); });
    std::set<ImGuiID> covers;
    const float font_size = ImGui::GetFontSize() * view.zoom;
    ImGui::PushFont(nullptr, std::max(font_size, 6.0F));
    std::string hovered_node;
    for (auto* ordered_node : ordered) {
        auto& node = *ordered_node;
        const auto top_left = to_screen({node.x, node.y});
        const auto bottom_right = to_screen({node.x + width_of(node), node.y + node_height(node)});
        const bool selected = view.selected.contains(node.id);
        const auto error = node_errors.find(node.id);
        const float rounding = 5.0F * scale;
        draw->AddRectFilled(top_left, bottom_right, IM_COL32(38, 40, 46, 240), rounding);
        draw->AddRectFilled(top_left, {bottom_right.x, top_left.y + header_height * scale}, header_color(node), rounding,
                            ImDrawFlags_RoundCornersTop);
        draw->AddRect(top_left, bottom_right,
                      error != node_errors.end() ? palette.danger : selected ? palette.accent : IM_COL32(0, 0, 0, 160),
                      rounding, 0, (selected || error != node_errors.end() ? 2.0F : 1.0F) * ui_scale);
        const auto title = shader_graph_node_title(graph, node);
        draw->AddText({top_left.x + 8.0F * scale, top_left.y + (header_height * scale - ImGui::GetFontSize()) * 0.5F},
                      palette.text, title.c_str());
        // An invisible cover over the node, so a node on top takes the pointer from the fields of
        // nodes beneath it; the canvas treats it as canvas.
        ImGui::SetCursorScreenPos(top_left);
        ImGui::PushID(node.id.c_str());
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##cover", {std::max(bottom_right.x - top_left.x, 1.0F), std::max(bottom_right.y - top_left.y, 1.0F)});
        covers.insert(ImGui::GetItemID());
        ImGui::PopID();
        if (note) note("shader_graph:node:" + node.id, top_left, bottom_right);
        if (note && error != node_errors.end()) note("shader_graph:error:" + node.id, top_left, bottom_right);
        if (ImGui::IsMouseHoveringRect(top_left, bottom_right)) hovered_node = node.id;
        if (node.kind != GraphNode::Kind::output) {
            const auto pin = output_pin(node);
            draw->AddCircleFilled(pin, pin_radius * scale, palette.text);
            if (!node.type.empty()) {
                const auto label_size = ImGui::CalcTextSize(node.type.c_str());
                draw->AddText({pin.x - label_size.x - 10.0F * scale, pin.y + header_height * scale * 0.5F + 2.0F * scale},
                              palette.text_faint, node.type.c_str());
            }
            if (note) note("shader_graph:out:" + node.id, {pin.x - 6.0F, pin.y - 6.0F}, {pin.x + 6.0F, pin.y + 6.0F});
        }
        if (header_rows(node)) {
            // The components a split reads or a set replaces.
            ImGui::SetCursorScreenPos({top_left.x + 8.0F * scale, top_left.y + (header_height + 2.0F) * scale});
            ImGui::PushID(node.id.c_str());
            ImGui::SetNextItemWidth(60.0F * scale);
            ImGui::SetNextItemAllowOverlap();
            char buffer[8]{};
            std::snprintf(buffer, sizeof(buffer), "%s", node.op.c_str());
            if (ImGui::InputText("##components", buffer, sizeof(buffer), ImGuiInputTextFlags_AutoSelectAll)) {
                const std::string text{buffer};
                if (valid_components(text) && text != node.op) {
                    node.op = text;
                    (void)update_graph_types(graph, stage);
                    result.changed = result.committed = true;
                }
            }
            if (note) note("shader_graph:components:" + node.id, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::PopID();
        }
        for (std::size_t index = 0; index < node.inputs.size(); ++index) {
            auto& input = node.inputs[index];
            const auto pin = input_pin(node, index);
            draw->AddCircleFilled(pin, pin_radius * scale,
                                  input.source.empty() ? IM_COL32(120, 120, 130, 255) : palette.text);
            const auto label = shader_graph_pin_label(node, index);
            draw->AddText({pin.x + 10.0F * scale, pin.y - ImGui::GetFontSize() * 0.5F}, palette.text_dim, label.c_str());
            if (note) note("shader_graph:in:" + node.id + ":" + input.name, {pin.x - 6.0F, pin.y - 6.0F}, {pin.x + 6.0F, pin.y + 6.0F});
            if (!input.source.empty()) continue;
            // A value for an unconnected pin, typed as GLSL (0.5, vec3(1.0, 0.0, 0.0), true...).
            // Output values sit at the node's right edge, clear of long names like NORMAL_MAP_DEPTH.
            const float field = (node.kind == GraphNode::Kind::output ? 84.0F : 96.0F) * scale;
            const float field_x = node.kind == GraphNode::Kind::output ? bottom_right.x - field - 6.0F * scale
                                                                       : pin.x + 72.0F * scale;
            ImGui::SetCursorScreenPos({field_x, pin.y - ImGui::GetFrameHeight() * 0.5F});
            ImGui::PushID((node.id + ':' + input.name).c_str());
            ImGui::SetNextItemWidth(field);
            ImGui::SetNextItemAllowOverlap();
            char buffer[96]{};
            std::snprintf(buffer, sizeof(buffer), "%s", input.value.c_str());
            std::string hint = "0.0";
            if (node.kind == GraphNode::Kind::output)
                for (const auto& builtin : graph_stage_outputs(graph.type, stage.name))
                    if (builtin.name == input.name) hint = builtin.value.empty() ? "unset" : std::string{builtin.value};
            ImGui::InputTextWithHint("##value", hint.c_str(), buffer, sizeof(buffer));
            if (ImGui::IsItemDeactivatedAfterEdit() && std::string{buffer} != input.value) {
                input.value = buffer;
                (void)update_graph_types(graph, stage);
                result.changed = result.committed = true;
            }
            if (note) note("shader_graph:value:" + node.id + ":" + input.name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::PopID();
        }
        if (error != node_errors.end() && ImGui::IsMouseHoveringRect(top_left, bottom_right) &&
            (ImGui::GetHoveredID() == canvas_id || covers.contains(ImGui::GetHoveredID())))
            ImGui::SetTooltip("%s", error->second.c_str());
    }
    ImGui::PopFont();

    // Hit tests for gestures that start on the canvas rather than on a value field.
    const auto mouse = io.MousePos;
    const float grab = std::max(8.0F * scale, 6.0F);
    const auto output_at = [&](const ImVec2 point) -> std::string {
        for (auto it = ordered.rbegin(); it != ordered.rend(); ++it)
            if ((*it)->kind != GraphNode::Kind::output && distance_squared(output_pin(**it), point) <= grab * grab)
                return (*it)->id;
        return {};
    };
    const auto input_at = [&](const ImVec2 point) -> std::optional<std::pair<std::string, std::size_t>> {
        for (auto it = ordered.rbegin(); it != ordered.rend(); ++it)
            for (std::size_t pin = 0; pin < (*it)->inputs.size(); ++pin)
                if (distance_squared(input_pin(**it, pin), point) <= grab * grab)
                    return std::pair{(*it)->id, pin};
        return std::nullopt;
    };
    const auto connect = [&](const std::string& from, const std::string& to, const std::size_t pin) {
        if (from == to) return;
        if (graph_would_cycle(stage, from, to)) {
            result.status = "That wire would make a loop";
            return;
        }
        auto* target = stage.find(to);
        if (!target || pin >= target->inputs.size()) return;
        target->inputs[pin].source = from;
        (void)update_graph_types(graph, stage);
        result.changed = result.committed = true;
    };
    const auto hovered_id = ImGui::GetHoveredID(), active_id = ImGui::GetActiveID();
    const bool canvas_hovered = hovered_id == canvas_id || active_id == canvas_id || covers.contains(hovered_id) ||
                                covers.contains(active_id);
    if (canvas_hovered && io.MouseWheel != 0.0F) {
        const auto before = to_canvas(mouse);
        view.zoom = std::clamp(view.zoom * (io.MouseWheel > 0.0F ? 1.12F : 1.0F / 1.12F), 0.3F, 2.0F);
        const float zoomed = view.zoom * ui_scale;
        view.pan = {mouse.x - origin.x - before.x * zoomed, mouse.y - origin.y - before.y * zoomed};
    }
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        view.press = mouse;
        if (auto from = output_at(mouse); !from.empty()) {
            view.drag = ShaderGraphView::Drag::wire;
            view.wire_from = from;
            view.wire_to.clear();
        } else if (const auto input = input_at(mouse)) {
            auto* node = stage.find(input->first);
            auto& pin = node->inputs[input->second];
            view.drag = ShaderGraphView::Drag::wire;
            if (!pin.source.empty()) {
                // Picking up a connected wire detaches it, to drop it somewhere else.
                view.wire_from = pin.source;
                view.wire_to.clear();
                pin.source.clear();
                (void)update_graph_types(graph, stage);
                result.changed = true;
            } else {
                view.wire_from.clear();
                view.wire_to = input->first;
                view.wire_pin = input->second;
            }
        } else if (!hovered_node.empty()) {
            std::erase(view.order, hovered_node);
            view.order.push_back(hovered_node);
            if (io.KeyCtrl) {
                if (!view.selected.erase(hovered_node)) view.selected.insert(hovered_node);
            } else if (!view.selected.contains(hovered_node)) {
                view.selected = {hovered_node};
            }
            view.drag = ShaderGraphView::Drag::nodes;
            view.moving.clear();
            for (const auto& id : view.selected)
                if (const auto* node = stage.find(id)) view.moving[id] = {node->x, node->y};
        } else {
            if (!io.KeyCtrl) view.selected.clear();
            view.drag = ShaderGraphView::Drag::box;
        }
    }
    if (canvas_hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        view.press = mouse;
        view.right_moved = false;
        view.drag = ShaderGraphView::Drag::pan;
    }
    switch (view.drag) {
    case ShaderGraphView::Drag::pan:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            if (distance_squared(mouse, view.press) > 16.0F) view.right_moved = true;
            view.pan.x += io.MouseDelta.x;
            view.pan.y += io.MouseDelta.y;
        } else {
            // A right click that did not pan opens a menu: the node's, or the add menu.
            if (!view.right_moved && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                view.context_node = hovered_node;
                view.menu_at = to_canvas(mouse);
                view.menu_wire_from.clear();
                if (hovered_node.empty() || hovered_node == "output") {
                    view.search.clear();
                    view.focus_search = true;
                    ImGui::OpenPopup("##add_node");
                } else {
                    ImGui::OpenPopup("##node_menu");
                }
            }
            view.drag = ShaderGraphView::Drag::none;
        }
        break;
    case ShaderGraphView::Drag::nodes:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 moved{(mouse.x - view.press.x) / scale, (mouse.y - view.press.y) / scale};
            for (const auto& [id, start] : view.moving)
                if (auto* node = stage.find(id)) {
                    node->x = start.x + moved.x;
                    node->y = start.y + moved.y;
                }
            if (moved.x != 0.0F || moved.y != 0.0F) result.changed = true;
        } else {
            if (distance_squared(mouse, view.press) > 1.0F) result.changed = result.committed = true;
            view.drag = ShaderGraphView::Drag::none;
        }
        break;
    case ShaderGraphView::Drag::wire: {
        const auto start = !view.wire_from.empty() && stage.find(view.wire_from) ? output_pin(*stage.find(view.wire_from))
                           : !view.wire_to.empty() && stage.find(view.wire_to)
                               ? input_pin(*stage.find(view.wire_to), view.wire_pin)
                               : mouse;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!view.wire_from.empty()) wire(start, mouse, palette.accent);
            else wire(mouse, start, palette.accent);
        } else {
            if (!view.wire_from.empty()) {
                if (const auto target = input_at(mouse)) {
                    connect(view.wire_from, target->first, target->second);
                } else if (hovered_node.empty() && distance_squared(mouse, view.press) > 64.0F) {
                    // Dropped on empty space: choose a node to connect it to.
                    view.menu_at = to_canvas(mouse);
                    view.menu_wire_from = view.wire_from;
                    view.search.clear();
                    view.focus_search = true;
                    ImGui::OpenPopup("##add_node");
                    result.committed = result.changed;
                } else {
                    result.committed = result.changed;
                }
            } else if (const auto from = output_at(mouse); !from.empty()) {
                connect(from, view.wire_to, view.wire_pin);
            }
            view.drag = ShaderGraphView::Drag::none;
        }
        break;
    }
    case ShaderGraphView::Drag::box:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            draw->AddRectFilled(view.press, mouse, IM_COL32(90, 140, 220, 40));
            draw->AddRect(view.press, mouse, palette.accent);
        } else {
            const ImVec2 low{std::min(view.press.x, mouse.x), std::min(view.press.y, mouse.y)};
            const ImVec2 high{std::max(view.press.x, mouse.x), std::max(view.press.y, mouse.y)};
            if (high.x - low.x > 3.0F || high.y - low.y > 3.0F)
                for (const auto& node : stage.nodes) {
                    const auto a = to_screen({node.x, node.y});
                    const auto b = to_screen({node.x + width_of(node), node.y + node_height(node)});
                    if (a.x < high.x && b.x > low.x && a.y < high.y && b.y > low.y) view.selected.insert(node.id);
                }
            view.drag = ShaderGraphView::Drag::none;
        }
        break;
    case ShaderGraphView::Drag::none: break;
    }
    draw->PopClipRect();

    // Deleting removes the selection (never the output) and the wires into and out of it.
    const auto remove_selected = [&] {
        std::erase_if(stage.nodes, [&](const GraphNode& node) {
            return view.selected.contains(node.id) && node.kind != GraphNode::Kind::output;
        });
        for (auto& node : stage.nodes)
            for (auto& input : node.inputs)
                if (!input.source.empty() && !stage.find(input.source)) input.source.clear();
        view.selected.clear();
        (void)update_graph_types(graph, stage);
        result.changed = result.committed = true;
    };
    // Copying keeps the copies' wires between themselves; pasting places them at `at`, selected.
    auto& clipboard = shader_graph_clipboard();
    const auto copy_selected = [&] {
        auto copied = copy_graph_nodes(graph, stage, {view.selected.begin(), view.selected.end()});
        if (copied.empty()) return;
        clipboard = std::move(copied);
        result.status = std::to_string(clipboard.nodes.size()) + (clipboard.nodes.size() == 1U ? " node" : " nodes") + " copied";
    };
    const auto place = [&](const GraphClipboard& nodes, const ImVec2 at, const bool keep_outside_wires) {
        const auto uniforms = graph.uniforms.size();
        const auto added = paste_graph_nodes(graph, stage, nodes, at.x, at.y, keep_outside_wires);
        if (added.empty()) return;
        view.selected = {added.begin(), added.end()};
        for (const auto& id : added) {
            std::erase(view.order, id);
            view.order.push_back(id);
        }
        result.changed = result.committed = true;
        if (graph.uniforms.size() > uniforms)
            result.status = "Pasted, adding the parameters this shader did not have";
    };
    const auto duplicate_selected = [&] {
        const auto copied = copy_graph_nodes(graph, stage, {view.selected.begin(), view.selected.end()});
        // Just below the originals, so neither covers the other.
        float left = std::numeric_limits<float>::max(), bottom = std::numeric_limits<float>::lowest();
        for (const auto& node : copied.nodes) {
            left = std::min(left, node.x);
            bottom = std::max(bottom, node.y + node_height(node));
        }
        if (!copied.empty()) place(copied, {left, bottom + 24.0F}, true);
    };
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput;
    if (focused && !view.selected.empty() &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
        remove_selected();
    if (focused && io.KeyCtrl && !view.selected.empty() && ImGui::IsKeyPressed(ImGuiKey_C, false)) copy_selected();
    if (focused && io.KeyCtrl && !view.selected.empty() && ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        copy_selected();
        remove_selected();
    }
    if (focused && io.KeyCtrl && !view.selected.empty() && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicate_selected();
    if (focused && io.KeyCtrl && !clipboard.empty() && ImGui::IsKeyPressed(ImGuiKey_V, false))
        place(clipboard, canvas_hovered ? to_canvas(mouse) : to_canvas({origin.x + size.x * 0.3F, origin.y + size.y * 0.3F}),
              false);
    if (focused && ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.KeyCtrl) view.frame_pending = true;
    if (focused && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        view.menu_at = to_canvas(mouse);
        view.menu_wire_from.clear();
        view.search.clear();
        view.focus_search = true;
        ImGui::OpenPopup("##add_node");
    }

    if (ImGui::BeginPopup("##node_menu")) {
        // The menu acts on the selection when the clicked node is in it, else on that node.
        if (!view.selected.contains(view.context_node)) view.selected = {view.context_node};
        if (ImGui::MenuItem("Copy", "Ctrl+C")) copy_selected();
        if (note) note("shader_graph:menu:copy", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selected();
        if (note) note("shader_graph:menu:duplicate", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::MenuItem("Delete", "Del")) remove_selected();
        if (note) note("shader_graph:menu:delete", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::MenuItem("Disconnect inputs"))
            if (auto* node = stage.find(view.context_node)) {
                for (auto& input : node->inputs) input.source.clear();
                (void)update_graph_types(graph, stage);
                result.changed = result.committed = true;
            }
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowSizeConstraints({260.0F * ui_scale, 0.0F}, {360.0F * ui_scale, 420.0F * ui_scale});
    if (ImGui::BeginPopup("##add_node")) {
        if (view.focus_search) {
            ImGui::SetKeyboardFocusHere();
            view.focus_search = false;
        }
        if (!clipboard.empty()) {
            const auto label = "Paste " + std::to_string(clipboard.nodes.size()) +
                               (clipboard.nodes.size() == 1U ? " node" : " nodes");
            if (ImGui::Selectable(label.c_str())) {
                place(clipboard, view.menu_at, false);
                view.menu_wire_from.clear();
                ImGui::CloseCurrentPopup();
            }
            if (note) note("shader_graph:add:paste", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::Separator();
        }
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "%s", view.search.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##search", "Search nodes", buffer, sizeof(buffer))) view.search = buffer;
        if (note) note("shader_graph:add:search", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        const auto query = lower(view.search);
        const auto entries = shader_graph_entries(graph, stage);
        const auto add = [&](const ShaderGraphEntry& entry) {
            const auto id = add_shader_graph_node(graph, stage, entry, view.menu_at);
            // A wire dropped on empty space feeds the new node's first input.
            if (!view.menu_wire_from.empty())
                if (auto* node = stage.find(id); node && !node->inputs.empty())
                    connect(view.menu_wire_from, id, 0U);
            view.selected = {id};
            std::erase(view.order, id);
            view.order.push_back(id);
            view.menu_wire_from.clear();
            result.changed = result.committed = true;
            ImGui::CloseCurrentPopup();
        };
        bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        ImGui::BeginChild("##entries", {0.0F, 300.0F * ui_scale});
        std::string category;
        for (const auto& entry : entries) {
            if (!query.empty() && lower(entry.label).find(query) == std::string::npos &&
                lower(entry.category).find(query) == std::string::npos)
                continue;
            if (query.empty() && entry.category != category) {
                category = entry.category;
                ImGui::SeparatorText(category.c_str());
            }
            ImGui::PushID((entry.category + entry.label).c_str());
            if (ImGui::Selectable(entry.label.c_str()) || enter) {
                enter = false;
                add(entry);
            }
            if (ImGui::IsItemHovered() && !entry.description.empty()) ImGui::SetTooltip("%s", entry.description.c_str());
            if (note) note("shader_graph:add:" + entry.label, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    } else if (!view.menu_wire_from.empty() && !ImGui::IsPopupOpen("##add_node")) {
        view.menu_wire_from.clear();
    }
    ImGui::SetCursorScreenPos({origin.x, origin.y + size.y});
    return result;
}

} // namespace relay
