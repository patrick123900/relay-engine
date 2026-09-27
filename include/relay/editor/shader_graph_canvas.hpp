#pragma once

#include "relay/render/shader_graph.hpp"

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace relay {

// What one stage's node canvas remembers between frames: where it looks, what is selected, and
// the gesture in progress.
struct ShaderGraphView {
    ImVec2 pan{40.0F, 40.0F};
    float zoom{1.0F};
    std::set<std::string> selected;
    enum class Drag : std::uint8_t { none, nodes, wire, box, pan } drag{Drag::none};
    // A wire being dragged from an output (`wire_from`) or back from an input (`wire_to`, `wire_pin`).
    std::string wire_from;
    std::string wire_to;
    std::size_t wire_pin{};
    ImVec2 press{};
    std::map<std::string, ImVec2> moving;
    bool right_moved{};
    // The add-node menu: where new nodes go, and a wire dropped on empty space to connect them to.
    ImVec2 menu_at{};
    std::string menu_wire_from;
    std::string search;
    bool focus_search{};
    std::string context_node;
    bool frame_pending{true};
    // Drawing order: nodes clicked last are drawn, and take clicks, on top.
    std::vector<std::string> order;
};

struct ShaderGraphCanvasResult {
    bool changed{};   // The graph changed this frame.
    bool committed{}; // A change finished (not a drag in progress): remember it and preview it.
    std::string status;
};

// Records a widget's rectangle for headless tests.
using ShaderGraphNote = std::function<void(const std::string& key, ImVec2 minimum, ImVec2 maximum)>;

// Draws a stage's nodes and wires in the remaining space of the current window and applies what
// the person does to `graph`. `node_errors` maps node ids to compile errors shown on them.
ShaderGraphCanvasResult draw_shader_graph_canvas(ShaderGraph& graph, GraphStage& stage, ShaderGraphView& view,
                                                 const std::map<std::string, std::string>& node_errors,
                                                 const ShaderGraphNote& note, float ui_scale);

// The menu entries for adding nodes to a stage: inputs, parameters, operators and functions.
struct ShaderGraphEntry {
    std::string label;
    std::string category;
    std::string description;
    GraphNode::Kind kind{GraphNode::Kind::call};
    std::string op;
    std::size_t inputs{};
    std::string default_value;
};
[[nodiscard]] std::vector<ShaderGraphEntry> shader_graph_entries(const ShaderGraph& graph, const GraphStage& stage);
// Adds a node for an entry at a canvas position; inputs and parameters already present are reused.
// Returns the node's id.
std::string add_shader_graph_node(ShaderGraph& graph, GraphStage& stage, const ShaderGraphEntry& entry, ImVec2 at);
// Nodes copied in any shader tab (Ctrl+C), for pasting into any other (Ctrl+V).
[[nodiscard]] GraphClipboard& shader_graph_clipboard();
// A node's title, and the label of one of its input pins.
[[nodiscard]] std::string shader_graph_node_title(const ShaderGraph& graph, const GraphNode& node);
[[nodiscard]] std::string shader_graph_pin_label(const GraphNode& node, std::size_t pin);

} // namespace relay
