#pragma once

#include "relay/render/shader_language.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace relay {

// Shader graphs: the node view of a .relay-shader file. The shading language is the saved form of
// every graph, so people edit nodes while agents read and write code, and both work on one file.
//
// shader_to_graph() turns each stage function (vertex(), fragment()) into dataflow: local variables,
// arithmetic, comparisons, swizzles, function calls and the built-ins a stage assigns become nodes
// and wires; literal arguments become values on unconnected pins. A stage that uses control flow
// (if, for, return...) stays code, shown as one code block; other top-level code (helper functions,
// constants) is kept as the graph's functions, callable from nodes. graph_to_shader() writes the
// graph back as one local variable per node, then the stage's outputs, and stores node positions in
// a final `// relay-graph {...}` comment, which the compiler ignores.

struct GraphInput {
    std::string name;   // Pin: a/b for operators, 1, 2... for call arguments, a built-in for outputs.
    std::string source; // The node feeding this pin; empty when it is not connected.
    std::string value;  // A GLSL constant used when not connected; empty for none.
    auto operator<=>(const GraphInput&) const = default;
};

struct GraphNode {
    enum class Kind : std::uint8_t {
        input,     // op: a built-in variable the stage reads (UV, TIME...)
        parameter, // op: a uniform's name
        binary,    // op: + - * / % < > <= >= == != && ||
        unary,     // op: - !
        select,    // condition ? a : b
        swizzle,   // op: the components read, such as rgb or y
        set,       // op: the components replaced in a vector, such as y
        call,      // op: a function or constructor name (sin, mix, texture, vec3...)
        output,    // the stage's results; one pin per built-in it may assign
    };
    std::string id;
    Kind kind{Kind::call};
    std::string op;
    std::vector<GraphInput> inputs;
    std::string type; // GLSL type of the value; empty when it cannot be worked out.
    float x{};
    float y{};
    auto operator<=>(const GraphNode&) const = default;
};

struct GraphStage {
    GraphStage() = default;
    explicit GraphStage(std::string stage_name) : name(std::move(stage_name)) {}
    std::string name;  // vertex or fragment
    bool present{};    // The file defines this function.
    bool code_only{};  // Kept as code; `code` holds the function's body.
    std::string code;
    std::string reason; // Why it could not become nodes.
    std::vector<GraphNode> nodes;
    [[nodiscard]] const GraphNode* find(std::string_view id) const;
    [[nodiscard]] GraphNode* find(std::string_view id);
    auto operator<=>(const GraphStage&) const = default;
};

struct ShaderGraph {
    ShaderType type{ShaderType::surface};
    bool transparent{};
    bool unshaded{};
    std::vector<ShaderUniform> uniforms;
    std::string functions; // Other top-level code, verbatim.
    GraphStage vertex{"vertex"};
    GraphStage fragment{"fragment"};
    // Problems in the file's declarations; while there are any the graph cannot be edited.
    std::vector<ShaderMessage> errors;
    [[nodiscard]] GraphStage* stage(std::string_view name);
};

// A built-in variable a stage reads or writes.
struct GraphBuiltin {
    std::string_view name;
    std::string_view type;
    std::string_view value;       // Outputs: the value it has when the shader leaves it alone.
    std::string_view description;
};
[[nodiscard]] std::vector<GraphBuiltin> graph_stage_inputs(ShaderType type, std::string_view stage);
[[nodiscard]] std::vector<GraphBuiltin> graph_stage_outputs(ShaderType type, std::string_view stage);

// Functions the node menu offers. `result` is a type, or "same" for GLSL's generic functions,
// whose result has the widest type of their arguments.
struct GraphFunction {
    std::string_view name;
    std::string_view category;
    std::vector<std::string_view> parameters;
    std::string_view result;
    std::string_view description;
};
[[nodiscard]] const std::vector<GraphFunction>& graph_functions();
[[nodiscard]] const GraphFunction* find_graph_function(std::string_view name, std::size_t arguments);

[[nodiscard]] ShaderGraph shader_to_graph(std::string_view text);

struct GraphSource {
    std::string text;
    // The node each line of the text came from, so compile errors can be shown on nodes.
    std::map<std::uint32_t, std::pair<std::string, std::string>> node_lines; // line -> (stage, node)
};
[[nodiscard]] GraphSource graph_to_shader(const ShaderGraph& graph);

// Works out every node's type from its inputs; returns the ids of nodes whose type is unknown.
std::vector<std::string> update_graph_types(ShaderGraph& graph, GraphStage& stage);
// Whether wiring `from` into `to` would make a loop.
[[nodiscard]] bool graph_would_cycle(const GraphStage& stage, std::string_view from, std::string_view to);
// Places nodes in columns by how far they are from the stage's inputs, the output on the right.
void layout_graph_stage(GraphStage& stage);
// A node id not used in the stage, from `stem` (n1, n2...).
[[nodiscard]] std::string unused_graph_id(const GraphStage& stage, std::string_view stem = "n");
// Declaration text for a uniform, as graph_to_shader writes it.
[[nodiscard]] std::string uniform_declaration(const ShaderUniform& uniform);

// Copied nodes, for pasting into this or another graph. Wires between the copied nodes are kept;
// `uniforms` holds the declarations of the parameters they use, added to a graph that lacks them.
// The output node is never copied.
struct GraphClipboard {
    ShaderType type{ShaderType::surface};
    std::vector<GraphNode> nodes;
    std::vector<ShaderUniform> uniforms;
    [[nodiscard]] bool empty() const { return nodes.empty(); }
};
[[nodiscard]] GraphClipboard copy_graph_nodes(const ShaderGraph& graph, const GraphStage& stage,
                                              const std::vector<std::string>& ids);
// Adds the clipboard's nodes to `stage`, keeping their arrangement with its top-left corner at
// (x, y), and returns the new nodes' ids. Built-in inputs and parameters the stage already shows
// are reused rather than repeated, and inputs this stage does not have are left out. Wires from
// nodes that were not copied are kept when `keep_outside_wires` (duplicating in place) and
// dropped otherwise (pasting).
std::vector<std::string> paste_graph_nodes(ShaderGraph& graph, GraphStage& stage, const GraphClipboard& clipboard,
                                           float x, float y, bool keep_outside_wires);

} // namespace relay
