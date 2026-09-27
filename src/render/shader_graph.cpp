#include "relay/render/shader_graph.hpp"

#include "relay/core/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace relay {
namespace {

// ---------------------------------------------------------------------------------------------
// Expressions: a GLSL subset parsed from a stage function's statements.

struct Token {
    enum class Kind : std::uint8_t { identifier, number, symbol, end } kind{Kind::end};
    std::string text;
};

std::vector<Token> tokenize(const std::string_view text, bool& ok) {
    std::vector<Token> result;
    ok = true;
    static const std::vector<std::string_view> symbols{"<<=", ">>=", "&&", "||", "^^", "==", "!=", "<=",
                                                       ">=", "+=", "-=", "*=", "/=", "%=", "++", "--"};
    for (std::size_t index = 0; index < text.size();) {
        const char c = text[index];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++index;
        } else if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
            const auto start = index;
            while (index < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[index])) != 0 || text[index] == '_'))
                ++index;
            result.push_back({Token::Kind::identifier, std::string{text.substr(start, index - start)}});
        } else if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
                   (c == '.' && index + 1U < text.size() && std::isdigit(static_cast<unsigned char>(text[index + 1U])) != 0)) {
            const auto start = index;
            while (index < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[index])) != 0 || text[index] == '.' ||
                    ((text[index] == '-' || text[index] == '+') &&
                     (text[index - 1U] == 'e' || text[index - 1U] == 'E'))))
                ++index;
            result.push_back({Token::Kind::number, std::string{text.substr(start, index - start)}});
        } else {
            std::string symbol(1U, c);
            for (const auto candidate : symbols)
                if (text.substr(index, candidate.size()) == candidate) {
                    symbol = std::string{candidate};
                    break;
                }
            index += symbol.size();
            if (std::string_view{"+-*/%<>=!&|^?:.,()[]{};"}.find(symbol[0]) == std::string_view::npos) ok = false;
            result.push_back({Token::Kind::symbol, symbol});
        }
    }
    result.push_back({Token::Kind::end, {}});
    return result;
}

struct Expr {
    enum class Kind : std::uint8_t { number, boolean, identifier, call, member, binary, unary, select };
    Kind kind{Kind::number};
    std::string text; // Number, name, function, member, operator.
    std::vector<Expr> children;
};

class ExpressionParser {
public:
    explicit ExpressionParser(const std::vector<Token>& tokens, const std::size_t start = 0U)
        : tokens_(tokens), at_(start) {}

    std::optional<Expr> parse() {
        auto result = conditional();
        if (!result || failed_) return std::nullopt;
        return result;
    }
    [[nodiscard]] std::size_t position() const { return at_; }

private:
    const Token& peek() const { return tokens_[std::min(at_, tokens_.size() - 1U)]; }
    bool accept(const std::string_view symbol) {
        if (peek().kind == Token::Kind::symbol && peek().text == symbol) {
            ++at_;
            return true;
        }
        return false;
    }

    std::optional<Expr> conditional() {
        auto condition = binary(0);
        if (!condition) return std::nullopt;
        if (!accept("?")) return condition;
        auto yes = conditional();
        if (!yes || !accept(":")) return fail();
        auto no = conditional();
        if (!no) return std::nullopt;
        Expr select{Expr::Kind::select, "?", {std::move(*condition), std::move(*yes), std::move(*no)}};
        return select;
    }

    static int precedence(const std::string& op) {
        if (op == "||") return 1;
        if (op == "^^") return 2;
        if (op == "&&") return 3;
        if (op == "==" || op == "!=") return 4;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 5;
        if (op == "+" || op == "-") return 6;
        if (op == "*" || op == "/" || op == "%") return 7;
        return -1;
    }

    std::optional<Expr> binary(const int minimum) {
        auto left = unary();
        if (!left) return std::nullopt;
        while (peek().kind == Token::Kind::symbol) {
            const auto op = peek().text;
            const int level = precedence(op);
            if (level < 0 || level < minimum) break;
            ++at_;
            auto right = binary(level + 1);
            if (!right) return std::nullopt;
            Expr combined{Expr::Kind::binary, op, {std::move(*left), std::move(*right)}};
            left = std::move(combined);
        }
        return left;
    }

    std::optional<Expr> unary() {
        if (peek().kind == Token::Kind::symbol && (peek().text == "-" || peek().text == "!" || peek().text == "+")) {
            const auto op = peek().text;
            ++at_;
            auto operand = unary();
            if (!operand) return std::nullopt;
            if (op == "+") return operand;
            Expr result{Expr::Kind::unary, op, {std::move(*operand)}};
            return result;
        }
        return postfix();
    }

    std::optional<Expr> postfix() {
        auto value = primary();
        if (!value) return std::nullopt;
        while (true) {
            if (accept(".")) {
                if (peek().kind != Token::Kind::identifier) return fail();
                Expr member{Expr::Kind::member, peek().text, {std::move(*value)}};
                ++at_;
                value = std::move(member);
            } else if (peek().kind == Token::Kind::symbol && (peek().text == "[" || peek().text == "++" ||
                                                               peek().text == "--")) {
                return fail(); // Indexing and increments stay code.
            } else {
                break;
            }
        }
        return value;
    }

    std::optional<Expr> primary() {
        const auto token = peek();
        if (token.kind == Token::Kind::number) {
            ++at_;
            return Expr{Expr::Kind::number, token.text, {}};
        }
        if (token.kind == Token::Kind::identifier) {
            ++at_;
            if (token.text == "true" || token.text == "false") return Expr{Expr::Kind::boolean, token.text, {}};
            if (accept("(")) {
                Expr call{Expr::Kind::call, token.text, {}};
                if (!accept(")")) {
                    do {
                        auto argument = conditional();
                        if (!argument) return std::nullopt;
                        call.children.push_back(std::move(*argument));
                    } while (accept(","));
                    if (!accept(")")) return fail();
                }
                return call;
            }
            return Expr{Expr::Kind::identifier, token.text, {}};
        }
        if (accept("(")) {
            auto inner = conditional();
            if (!inner || !accept(")")) return fail();
            return inner;
        }
        return fail();
    }

    std::optional<Expr> fail() {
        failed_ = true;
        return std::nullopt;
    }

    const std::vector<Token>& tokens_;
    std::size_t at_{};
    bool failed_{};
};

// ---------------------------------------------------------------------------------------------
// Types.

const std::set<std::string, std::less<>> constructors{
    "float", "int", "uint", "bool", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2",
    "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4"};
const std::set<std::string, std::less<>> declaration_types{
    "float", "int", "uint", "bool", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2",
    "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4"};

int vector_width(const std::string_view type) {
    if (type == "float" || type == "int" || type == "uint" || type == "bool") return 1;
    if (type.size() >= 4U && std::isdigit(static_cast<unsigned char>(type.back())) != 0 &&
        type.find("vec") != std::string_view::npos)
        return type.back() - '0';
    return 0;
}

bool is_matrix(const std::string_view type) { return type.starts_with("mat"); }

// The wider of two arithmetic types, as GLSL's generic functions and operators use it.
std::string widest(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (is_matrix(a) && vector_width(b) > 1) return b;
    if (is_matrix(b) && vector_width(a) > 1) return a;
    if (is_matrix(a)) return a;
    if (is_matrix(b)) return b;
    const int wa = vector_width(a), wb = vector_width(b);
    if (wa == wb) return (a == "int" && b == "float") ? b : a;
    return wa > wb ? a : b;
}

bool is_swizzle(const std::string_view text) {
    if (text.empty() || text.size() > 4U) return false;
    for (const auto set : {std::string_view{"xyzw"}, std::string_view{"rgba"}, std::string_view{"stpq"}})
        if (std::all_of(text.begin(), text.end(), [&](char c) { return set.find(c) != std::string_view::npos; }))
            return true;
    return false;
}

std::string swizzle_type(const std::string_view components) {
    return components.size() == 1U ? std::string{"float"} : "vec" + std::to_string(components.size());
}

std::string constant_type(const std::string_view text) {
    if (text == "true" || text == "false") return "bool";
    const auto open = text.find('(');
    if (open != std::string_view::npos) {
        const auto name = std::string{text.substr(0U, open)};
        if (constructors.contains(name)) return name;
        return {};
    }
    std::string_view number = text;
    while (!number.empty() && (number.front() == '-' || number.front() == ' ')) number.remove_prefix(1U);
    if (!number.empty() && (std::isdigit(static_cast<unsigned char>(number.front())) != 0 || number.front() == '.')) {
        if (number.find_first_of(".eE") != std::string_view::npos) return "float";
        if (number.back() == 'u' || number.back() == 'U') return "uint";
        return number.find_first_of(" *+/") == std::string_view::npos ? "int" : "float";
    }
    return {};
}

// Return types of the project's own functions, from their definitions.
std::map<std::string, std::string, std::less<>> function_signatures(const std::string_view functions) {
    std::map<std::string, std::string, std::less<>> result;
    const auto parsed = parse_relay_shader("shader_type surface;\n" + std::string{functions});
    for (const auto& function : parsed.functions) {
        bool ok = true;
        const auto head = tokenize(std::string_view{parsed.code}.substr(function.begin, function.end - function.begin), ok);
        for (std::size_t index = 0; index + 1U < head.size(); ++index)
            if (head[index + 1U].text == function.name && head[index].kind == Token::Kind::identifier) {
                result[function.name] = head[index].text;
                break;
            }
    }
    return result;
}

std::string render_expression(const Expr& expr, const bool nested = false) {
    switch (expr.kind) {
    case Expr::Kind::number:
    case Expr::Kind::boolean:
    case Expr::Kind::identifier: return expr.text;
    case Expr::Kind::call: {
        std::string text = expr.text + '(';
        for (std::size_t index = 0; index < expr.children.size(); ++index)
            text += (index ? ", " : "") + render_expression(expr.children[index]);
        return text + ')';
    }
    case Expr::Kind::member: return render_expression(expr.children[0], true) + '.' + expr.text;
    case Expr::Kind::unary: return expr.text + render_expression(expr.children[0], true);
    case Expr::Kind::binary: {
        auto text = render_expression(expr.children[0], true) + ' ' + expr.text + ' ' +
                    render_expression(expr.children[1], true);
        return nested ? '(' + text + ')' : text;
    }
    case Expr::Kind::select: {
        auto text = render_expression(expr.children[0], true) + " ? " + render_expression(expr.children[1], true) +
                    " : " + render_expression(expr.children[2], true);
        return nested ? '(' + text + ')' : text;
    }
    }
    return expr.text;
}

// ---------------------------------------------------------------------------------------------
// Stage conversion.

struct Value {
    std::string node;     // A node id...
    std::string constant; // ...or constant GLSL text.
};

class StageBuilder {
public:
    StageBuilder(const ShaderGraph& graph, GraphStage& stage) : graph_(graph), stage_(stage) {
        for (const auto& builtin : graph_stage_inputs(graph.type, stage.name)) readable_.insert(std::string{builtin.name});
        for (const auto& builtin : graph_stage_outputs(graph.type, stage.name)) {
            outputs_.emplace_back(builtin.name);
            defaults_[std::string{builtin.name}] = std::string{builtin.value};
        }
        for (const auto& uniform : graph.uniforms) uniforms_.insert(uniform.name);
    }

    // Converts the body; false (with a reason) when it has to stay code.
    bool build(const std::string_view body, std::string& reason) {
        bool ok = true;
        const auto all = tokenize(body, ok);
        if (!ok) {
            reason = "it uses symbols the graph does not have";
            return false;
        }
        std::size_t start = 0U;
        for (std::size_t index = 0; index < all.size(); ++index) {
            if (all[index].kind == Token::Kind::symbol && (all[index].text == "{" || all[index].text == "}")) {
                reason = "it uses blocks such as if or for";
                return false;
            }
            if (all[index].kind != Token::Kind::end &&
                !(all[index].kind == Token::Kind::symbol && all[index].text == ";"))
                continue;
            std::vector<Token> statement(all.begin() + static_cast<std::ptrdiff_t>(start),
                                         all.begin() + static_cast<std::ptrdiff_t>(index));
            start = index + 1U;
            if (statement.empty()) continue;
            statement.push_back({Token::Kind::end, {}});
            if (!this->statement(statement, reason)) return false;
        }
        // The output node collects what the stage assigned.
        GraphNode output;
        output.id = "output";
        output.kind = GraphNode::Kind::output;
        output.op = stage_.name;
        for (const auto& name : outputs_) {
            GraphInput pin{name, {}, {}};
            if (const auto found = assigned_.find(name); found != assigned_.end()) {
                pin.source = found->second.node;
                pin.value = found->second.constant;
            }
            output.inputs.push_back(std::move(pin));
        }
        stage_.nodes.push_back(std::move(output));
        return true;
    }

private:
    bool statement(const std::vector<Token>& tokens, std::string& reason) {
        static const std::set<std::string, std::less<>> keywords{"if", "else", "for", "while", "do", "switch",
                                                                 "return", "discard", "break", "continue", "case"};
        std::size_t at = 0U;
        if (tokens[at].kind == Token::Kind::identifier && keywords.contains(tokens[at].text)) {
            reason = "it uses " + tokens[at].text;
            return false;
        }
        if (tokens[at].text == "const" || tokens[at].text == "highp" || tokens[at].text == "mediump" ||
            tokens[at].text == "lowp")
            ++at;
        // Declaration: type name = expression.
        if (tokens[at].kind == Token::Kind::identifier && declaration_types.contains(tokens[at].text) &&
            tokens[at + 1U].kind == Token::Kind::identifier) {
            const auto name = tokens[at + 1U].text;
            if (tokens[at + 2U].text != "=") {
                reason = "a variable is declared without a value";
                return false;
            }
            ExpressionParser parser(tokens, at + 3U);
            const auto expression = parser.parse();
            if (!expression || tokens[parser.position()].kind != Token::Kind::end) {
                reason = "an expression is not one the graph can show";
                return false;
            }
            const auto value = evaluate(*expression, name, reason);
            if (!value) return false;
            locals_[name] = *value;
            return true;
        }
        // Assignment: target[.components] op= expression.
        if (tokens[at].kind != Token::Kind::identifier) {
            reason = "a statement is not an assignment";
            return false;
        }
        const auto target = tokens[at].text;
        std::string components;
        std::size_t next = at + 1U;
        if (tokens[next].text == "." && tokens[next + 1U].kind == Token::Kind::identifier) {
            components = tokens[next + 1U].text;
            next += 2U;
            if (!is_swizzle(components)) {
                reason = "a statement assigns to a field";
                return false;
            }
        }
        const auto& op = tokens[next].text;
        if (op != "=" && op != "+=" && op != "-=" && op != "*=" && op != "/=") {
            reason = "a statement is not an assignment";
            return false;
        }
        const bool local = locals_.contains(target);
        const bool output = std::find(outputs_.begin(), outputs_.end(), target) != outputs_.end();
        if (!local && !output) {
            reason = target + " cannot be assigned";
            return false;
        }
        ExpressionParser parser(tokens, next + 1U);
        const auto expression = parser.parse();
        if (!expression || tokens[parser.position()].kind != Token::Kind::end) {
            reason = "an expression is not one the graph can show";
            return false;
        }
        // The new value, named after a local being written for the first time as its own node.
        const std::string preferred = local && !used_.contains(target) ? target : std::string{};
        std::optional<Value> value;
        if (op == "=" && components.empty()) {
            value = evaluate(*expression, preferred, reason);
        } else {
            auto current = read(target);
            Value base = current;
            // Only a compound assignment reads the components it replaces.
            if (!components.empty() && op != "=") base = swizzled(current, components);
            auto operand = evaluate(*expression, components.empty() ? preferred : std::string{}, reason);
            if (!operand) return false;
            value = operand;
            if (op != "=") {
                value = make(GraphNode::Kind::binary, std::string(1U, op[0]), {{"a", base}, {"b", *operand}},
                             components.empty() ? preferred : std::string{});
            }
            if (!components.empty())
                value = make(GraphNode::Kind::set, components, {{"vector", current}, {"value", *value}}, preferred);
        }
        if (!value) return false;
        if (local) locals_[target] = *value;
        else assigned_[target] = *value;
        return true;
    }

    // The value a name has at this point of the stage.
    Value read(const std::string& name) {
        if (const auto found = locals_.find(name); found != locals_.end()) return found->second;
        if (const auto found = assigned_.find(name); found != assigned_.end()) return found->second;
        if (readable_.contains(name)) return {node_for("input:" + name, GraphNode::Kind::input, name), {}};
        if (const auto found = defaults_.find(name); found != defaults_.end()) return {{}, found->second};
        if (uniforms_.contains(name)) return {node_for("uniform:" + name, GraphNode::Kind::parameter, name), {}};
        return {{}, name}; // A constant from the shader's functions, or a mistake the compiler reports.
    }

    std::string node_for(const std::string& id, const GraphNode::Kind kind, const std::string& op) {
        if (!stage_.find(id)) {
            GraphNode node;
            node.id = id;
            node.kind = kind;
            node.op = op;
            stage_.nodes.push_back(std::move(node));
        }
        return id;
    }

    Value swizzled(const Value& base, const std::string& components) {
        if (!base.constant.empty()) {
            const bool simple = base.constant.find_first_of(" ?") == std::string::npos;
            return {{}, (simple ? base.constant : '(' + base.constant + ')') + '.' + components};
        }
        return make(GraphNode::Kind::swizzle, components, {{"vector", base}}, {});
    }

    Value make(const GraphNode::Kind kind, std::string op, const std::vector<std::pair<std::string, Value>>& inputs,
               const std::string& preferred) {
        GraphNode node;
        node.kind = kind;
        node.op = std::move(op);
        node.id = !preferred.empty() && !stage_.find(preferred) ? preferred : unused_graph_id(stage_);
        used_.insert(node.id);
        for (const auto& [name, value] : inputs) node.inputs.push_back({name, value.node, value.constant});
        stage_.nodes.push_back(std::move(node));
        return {stage_.nodes.back().id, {}};
    }

    static bool constant_expression(const Expr& expr, const std::function<bool(const std::string&)>& known) {
        switch (expr.kind) {
        case Expr::Kind::number:
        case Expr::Kind::boolean: return true;
        case Expr::Kind::identifier: return !known(expr.text);
        case Expr::Kind::call:
            return constructors.contains(expr.text) &&
                   std::all_of(expr.children.begin(), expr.children.end(),
                               [&](const Expr& child) { return constant_expression(child, known); });
        default:
            return std::all_of(expr.children.begin(), expr.children.end(),
                               [&](const Expr& child) { return constant_expression(child, known); });
        }
    }

    std::optional<Value> evaluate(const Expr& expr, const std::string& preferred, std::string& reason) {
        const auto known = [&](const std::string& name) {
            return locals_.contains(name) || assigned_.contains(name) || readable_.contains(name) ||
                   defaults_.contains(name) || uniforms_.contains(name);
        };
        if (constant_expression(expr, known)) return Value{{}, render_expression(expr)};
        switch (expr.kind) {
        case Expr::Kind::identifier: return read(expr.text);
        case Expr::Kind::member: {
            if (!is_swizzle(expr.text)) {
                reason = "an expression reads a field";
                return std::nullopt;
            }
            const auto base = evaluate(expr.children[0], {}, reason);
            if (!base) return std::nullopt;
            if (!base->constant.empty()) return swizzled(*base, expr.text);
            return make(GraphNode::Kind::swizzle, expr.text, {{"vector", *base}}, preferred);
        }
        case Expr::Kind::call: {
            std::vector<std::pair<std::string, Value>> inputs;
            for (std::size_t index = 0; index < expr.children.size(); ++index) {
                const auto argument = evaluate(expr.children[index], {}, reason);
                if (!argument) return std::nullopt;
                inputs.emplace_back(std::to_string(index + 1U), *argument);
            }
            return make(GraphNode::Kind::call, expr.text, inputs, preferred);
        }
        case Expr::Kind::unary: {
            const auto operand = evaluate(expr.children[0], {}, reason);
            if (!operand) return std::nullopt;
            return make(GraphNode::Kind::unary, expr.text, {{"a", *operand}}, preferred);
        }
        case Expr::Kind::binary: {
            const auto a = evaluate(expr.children[0], {}, reason);
            const auto b = a ? evaluate(expr.children[1], {}, reason) : std::nullopt;
            if (!b) return std::nullopt;
            return make(GraphNode::Kind::binary, expr.text, {{"a", *a}, {"b", *b}}, preferred);
        }
        case Expr::Kind::select: {
            const auto condition = evaluate(expr.children[0], {}, reason);
            const auto yes = condition ? evaluate(expr.children[1], {}, reason) : std::nullopt;
            const auto no = yes ? evaluate(expr.children[2], {}, reason) : std::nullopt;
            if (!no) return std::nullopt;
            return make(GraphNode::Kind::select, "?", {{"condition", *condition}, {"a", *yes}, {"b", *no}}, preferred);
        }
        default: return Value{{}, render_expression(expr)};
        }
    }

    const ShaderGraph& graph_;
    GraphStage& stage_;
    std::set<std::string, std::less<>> readable_, uniforms_, used_;
    std::vector<std::string> outputs_;
    std::map<std::string, std::string, std::less<>> defaults_;
    std::map<std::string, Value, std::less<>> locals_, assigned_;
};

std::string trim_blank_lines(std::string text) {
    // Collapse runs of empty lines left by blanked declarations, and trim the ends.
    std::string result;
    std::istringstream lines{text};
    std::string line;
    int blank = 0;
    while (std::getline(lines, line)) {
        const bool empty = line.find_first_not_of(" \t\r") == std::string::npos;
        if (empty) {
            ++blank;
            continue;
        }
        if (!result.empty() && blank > 0) result += '\n';
        blank = 0;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
        result += line + '\n';
    }
    return result;
}

std::string number_literal(const double value) {
    std::ostringstream output;
    output << std::setprecision(6) << value;
    auto text = output.str();
    if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
    return text;
}

constexpr std::string_view layout_marker = "// relay-graph ";

} // namespace

const GraphNode* GraphStage::find(const std::string_view id) const {
    const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& node) { return node.id == id; });
    return found == nodes.end() ? nullptr : &*found;
}

GraphNode* GraphStage::find(const std::string_view id) {
    const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const GraphNode& node) { return node.id == id; });
    return found == nodes.end() ? nullptr : &*found;
}

GraphStage* ShaderGraph::stage(const std::string_view name) {
    return name == "vertex" ? &vertex : name == "fragment" ? &fragment : nullptr;
}

std::vector<GraphBuiltin> graph_stage_inputs(const ShaderType type, const std::string_view stage) {
    if (type == ShaderType::post_process) {
        if (stage != "fragment") return {};
        return {{"SCREEN_UV", "vec2", "", "This pixel's position on screen, 0 to 1"},
                {"COLOR", "vec3", "", "The lit scene's color at this pixel"},
                {"DEPTH", "float", "", "Distance to the surface here, in metres"},
                {"NORMAL", "vec3", "", "World-space normal here; zero on the sky"},
                {"MOTION", "vec2", "", "How far this pixel moved on screen since the last frame"},
                {"SCREEN_PIXEL_SIZE", "vec2", "", "One pixel in screen units"},
                {"TIME", "float", "", "Seconds"},
                {"DELTA_TIME", "float", "", "Seconds since the last frame"},
                {"FRAGCOORD", "vec4", "", "Pixel coordinates"},
                {"SCREEN_TEXTURE", "sampler2D", "", "The scene's colors, for texture()"}};
    }
    if (stage == "vertex")
        return {{"VERTEX", "vec3", "", "Position in the object's space"},
                {"NORMAL", "vec3", "", "Normal in the object's space"},
                {"UV", "vec2", "", "Texture coordinates"},
                {"TANGENT", "vec4", "", "Tangent, with the bitangent's sign in w"},
                {"TIME", "float", "", "Seconds"},
                {"MODEL_MATRIX", "mat4", "", "Object to world"}};
    return {{"UV", "vec2", "", "Texture coordinates"},
            {"NORMAL", "vec3", "", "World-space normal, facing the camera"},
            {"VIEW", "vec3", "", "Direction towards the camera"},
            {"WORLD_POSITION", "vec3", "", "Position in the world"},
            {"TIME", "float", "", "Seconds"},
            {"TANGENT", "vec4", "", "World-space tangent"},
            {"CAMERA_POSITION", "vec3", "", "The camera's position"},
            {"FRAGCOORD", "vec4", "", "Pixel coordinates"},
            {"FRONT_FACING", "bool", "", "Whether this is the front of the face"},
            {"MODEL_MATRIX", "mat4", "", "Object to world"}};
}

std::vector<GraphBuiltin> graph_stage_outputs(const ShaderType type, const std::string_view stage) {
    if (type == ShaderType::post_process)
        return stage == "fragment" ? std::vector<GraphBuiltin>{{"COLOR", "vec3", "", "The pixel's new color"}}
                                   : std::vector<GraphBuiltin>{};
    if (stage == "vertex")
        return {{"VERTEX", "vec3", "", "Moved position"},
                {"NORMAL", "vec3", "", "Changed normal"},
                {"UV", "vec2", "", "Changed texture coordinates"},
                {"TANGENT", "vec4", "", "Changed tangent"}};
    return {{"ALBEDO", "vec3", "vec3(1.0)", "Base color"},
            {"ALPHA", "float", "1.0", "Opacity, for transparent shaders"},
            {"METALLIC", "float", "0.0", "0 to 1"},
            {"ROUGHNESS", "float", "0.5", "0 (mirror) to 1"},
            {"EMISSION", "vec3", "vec3(0.0)", "Light the surface gives off"},
            {"AO", "float", "1.0", "Ambient occlusion"},
            {"NORMAL_MAP", "vec3", "vec3(0.5, 0.5, 1.0)", "Tangent-space normal, as a normal map stores it"},
            {"NORMAL_MAP_DEPTH", "float", "1.0", "Strength of the normal map"},
            {"ALPHA_SCISSOR_THRESHOLD", "float", "0.0", "Cut out pixels with less alpha"},
            {"NORMAL", "vec3", "", "Changed world-space normal"}};
}

const std::vector<GraphFunction>& graph_functions() {
    static const std::vector<GraphFunction> functions{
        {"abs", "Math", {"x"}, "same", "Absolute value"},
        {"sign", "Math", {"x"}, "same", "-1, 0 or 1"},
        {"floor", "Math", {"x"}, "same", "Round down"},
        {"ceil", "Math", {"x"}, "same", "Round up"},
        {"fract", "Math", {"x"}, "same", "Fractional part"},
        {"mod", "Math", {"x", "y"}, "same", "Remainder of x / y"},
        {"min", "Math", {"a", "b"}, "same", "Smaller of two values"},
        {"max", "Math", {"a", "b"}, "same", "Larger of two values"},
        {"clamp", "Math", {"x", "min", "max"}, "same", "Keep x between min and max"},
        {"mix", "Math", {"a", "b", "t"}, "same", "Blend from a to b by t"},
        {"step", "Math", {"edge", "x"}, "same", "0 below the edge, 1 from it"},
        {"smoothstep", "Math", {"edge0", "edge1", "x"}, "same", "Smooth 0 to 1 between two edges"},
        {"pow", "Math", {"x", "y"}, "same", "x to the power y"},
        {"exp", "Math", {"x"}, "same", "e to the power x"},
        {"exp2", "Math", {"x"}, "same", "2 to the power x"},
        {"log", "Math", {"x"}, "same", "Natural logarithm"},
        {"log2", "Math", {"x"}, "same", "Base-2 logarithm"},
        {"sqrt", "Math", {"x"}, "same", "Square root"},
        {"inversesqrt", "Math", {"x"}, "same", "1 / square root"},
        {"sin", "Trigonometry", {"angle"}, "same", "Sine (radians)"},
        {"cos", "Trigonometry", {"angle"}, "same", "Cosine (radians)"},
        {"tan", "Trigonometry", {"angle"}, "same", "Tangent (radians)"},
        {"asin", "Trigonometry", {"x"}, "same", "Arc sine"},
        {"acos", "Trigonometry", {"x"}, "same", "Arc cosine"},
        {"atan", "Trigonometry", {"y", "x"}, "same", "Angle of (x, y)"},
        {"radians", "Trigonometry", {"degrees"}, "same", "Degrees to radians"},
        {"degrees", "Trigonometry", {"radians"}, "same", "Radians to degrees"},
        {"length", "Vector", {"v"}, "float", "Length"},
        {"distance", "Vector", {"a", "b"}, "float", "Distance between two points"},
        {"dot", "Vector", {"a", "b"}, "float", "Dot product"},
        {"cross", "Vector", {"a", "b"}, "vec3", "Cross product"},
        {"normalize", "Vector", {"v"}, "same", "Length 1, same direction"},
        {"reflect", "Vector", {"incident", "normal"}, "same", "Reflect a direction off a surface"},
        {"refract", "Vector", {"incident", "normal", "eta"}, "same", "Bend a direction through a surface"},
        {"vec2", "Vector", {"x", "y"}, "vec2", "Combine into a vec2"},
        {"vec3", "Vector", {"x", "y", "z"}, "vec3", "Combine into a vec3"},
        {"vec4", "Vector", {"x", "y", "z", "w"}, "vec4", "Combine into a vec4"},
        {"texture", "Texture", {"image", "uv"}, "vec4", "Sample an image"},
        {"textureLod", "Texture", {"image", "uv", "level"}, "vec4", "Sample an image at a mip level"},
        {"dFdx", "Derivative", {"x"}, "same", "Change across the screen horizontally"},
        {"dFdy", "Derivative", {"x"}, "same", "Change across the screen vertically"},
        {"fwidth", "Derivative", {"x"}, "same", "Total change across a pixel"},
        {"scene_color", "Scene", {"uv"}, "vec3", "The lit scene's color at a screen position"},
        {"scene_color_lod", "Scene", {"uv", "lod"}, "vec3",
         "The scene's color blurred: level 0 is sharp, each level up twice as blurry"},
        {"scene_depth", "Scene", {"uv"}, "float", "Distance to the surface at a screen position"},
        {"scene_normal", "Scene", {"uv"}, "vec3", "The normal at a screen position"},
        {"scene_motion", "Scene", {"uv"}, "vec2", "How far a screen position moved since the last frame"},
    };
    return functions;
}

const GraphFunction* find_graph_function(const std::string_view name, const std::size_t arguments) {
    const GraphFunction* first = nullptr;
    for (const auto& function : graph_functions()) {
        if (function.name != name) continue;
        if (function.parameters.size() == arguments) return &function;
        if (!first) first = &function;
    }
    return first;
}

std::string unused_graph_id(const GraphStage& stage, const std::string_view stem) {
    for (std::size_t index = 1;; ++index) {
        auto id = std::string{stem} + std::to_string(index);
        if (!stage.find(id)) return id;
    }
}

GraphClipboard copy_graph_nodes(const ShaderGraph& graph, const GraphStage& stage, const std::vector<std::string>& ids) {
    GraphClipboard clipboard;
    clipboard.type = graph.type;
    // In the stage's order, so pasted ids and the written code keep the same sequence.
    for (const auto& node : stage.nodes) {
        if (node.kind == GraphNode::Kind::output || std::find(ids.begin(), ids.end(), node.id) == ids.end()) continue;
        clipboard.nodes.push_back(node);
        if (node.kind != GraphNode::Kind::parameter) continue;
        const auto uniform = std::find_if(graph.uniforms.begin(), graph.uniforms.end(),
                                          [&](const ShaderUniform& item) { return item.name == node.op; });
        if (uniform != graph.uniforms.end()) clipboard.uniforms.push_back(*uniform);
    }
    return clipboard;
}

std::vector<std::string> paste_graph_nodes(ShaderGraph& graph, GraphStage& stage, const GraphClipboard& clipboard,
                                           const float x, const float y, const bool keep_outside_wires) {
    std::vector<std::string> created;
    if (clipboard.empty()) return created;
    float left = std::numeric_limits<float>::max(), top = std::numeric_limits<float>::max();
    for (const auto& node : clipboard.nodes) {
        left = std::min(left, node.x);
        top = std::min(top, node.y);
    }
    const auto inputs = graph_stage_inputs(graph.type, stage.name);
    std::map<std::string, std::string> renamed; // Copied id -> id in this stage; empty when left out.
    std::vector<std::size_t> added;              // Indices in stage.nodes of the new nodes.
    for (const auto& original : clipboard.nodes) {
        auto node = original;
        node.x = x + (original.x - left);
        node.y = y + (original.y - top);
        if (node.kind == GraphNode::Kind::input || node.kind == GraphNode::Kind::parameter) {
            node.id = (node.kind == GraphNode::Kind::input ? "input:" : "uniform:") + node.op;
            renamed[original.id] = node.id;
            if (stage.find(node.id)) continue;
            if (node.kind == GraphNode::Kind::input &&
                std::none_of(inputs.begin(), inputs.end(), [&](const GraphBuiltin& item) { return item.name == node.op; })) {
                renamed[original.id].clear();
                continue;
            }
            if (node.kind == GraphNode::Kind::parameter &&
                std::none_of(graph.uniforms.begin(), graph.uniforms.end(),
                             [&](const ShaderUniform& item) { return item.name == node.op; })) {
                const auto uniform = std::find_if(clipboard.uniforms.begin(), clipboard.uniforms.end(),
                                                  [&](const ShaderUniform& item) { return item.name == node.op; });
                if (uniform == clipboard.uniforms.end()) {
                    renamed[original.id].clear();
                    continue;
                }
                graph.uniforms.push_back(*uniform);
            }
        } else {
            node.id = unused_graph_id(stage);
            renamed[original.id] = node.id;
        }
        added.push_back(stage.nodes.size());
        created.push_back(node.id);
        stage.nodes.push_back(std::move(node));
    }
    for (const auto index : added)
        for (auto& input : stage.nodes[index].inputs) {
            if (input.source.empty()) continue;
            if (const auto found = renamed.find(input.source); found != renamed.end()) input.source = found->second;
            else if (!keep_outside_wires || !stage.find(input.source)) input.source.clear();
        }
    (void)update_graph_types(graph, stage);
    return created;
}

std::string uniform_declaration(const ShaderUniform& uniform) {
    std::string text = "uniform " + std::string{shader_uniform_type_name(uniform.type)} + ' ' + uniform.name;
    switch (uniform.hint) {
    case ShaderUniform::Hint::color: text += " : source_color"; break;
    case ShaderUniform::Hint::normal: text += " : hint_normal"; break;
    case ShaderUniform::Hint::black: text += " : hint_black"; break;
    case ShaderUniform::Hint::white: text += " : hint_white"; break;
    case ShaderUniform::Hint::range:
        text += " : hint_range(" + number_literal(uniform.minimum) + ", " + number_literal(uniform.maximum) +
                (uniform.step > 0.0 ? ", " + number_literal(uniform.step) : std::string{}) + ')';
        break;
    case ShaderUniform::Hint::none: break;
    }
    const auto components = shader_uniform_components(uniform.type);
    if (components == 0U) return text + ';';
    const auto& value = uniform.default_value;
    if (uniform.type == ShaderUniform::Type::bool_value) return text + " = " + (value[0] != 0.0 ? "true" : "false") + ';';
    if (uniform.type == ShaderUniform::Type::int_value)
        return text + " = " + std::to_string(static_cast<long long>(std::llround(value[0]))) + ';';
    if (components == 1U) return text + " = " + number_literal(value[0]) + ';';
    text += " = " + std::string{shader_uniform_type_name(uniform.type)} + '(';
    for (std::size_t index = 0; index < components; ++index) text += (index ? ", " : "") + number_literal(value[index]);
    return text + ");";
}

std::vector<std::string> update_graph_types(ShaderGraph& graph, GraphStage& stage) {
    std::map<std::string, std::string, std::less<>> builtin_types;
    for (const auto& builtin : graph_stage_inputs(graph.type, stage.name))
        builtin_types[std::string{builtin.name}] = builtin.type;
    const auto signatures = function_signatures(graph.functions);
    std::vector<std::string> unknown;
    std::set<std::string, std::less<>> done;
    std::function<std::string(const std::string&)> type_of;
    const auto pin_type = [&](const GraphInput& pin) {
        return pin.source.empty() ? constant_type(pin.value) : type_of(pin.source);
    };
    type_of = [&](const std::string& id) -> std::string {
        auto* node = stage.find(id);
        if (!node) return {};
        if (done.contains(id)) return node->type;
        done.insert(id); // Guards against loops.
        std::string type;
        switch (node->kind) {
        case GraphNode::Kind::input: type = builtin_types[node->op]; break;
        case GraphNode::Kind::parameter:
            for (const auto& uniform : graph.uniforms)
                if (uniform.name == node->op) type = std::string{shader_uniform_type_name(uniform.type)};
            break;
        case GraphNode::Kind::binary:
            if (node->op == "<" || node->op == ">" || node->op == "<=" || node->op == ">=" || node->op == "==" ||
                node->op == "!=" || node->op == "&&" || node->op == "||" || node->op == "^^")
                type = "bool";
            else if (node->inputs.size() == 2U)
                type = widest(pin_type(node->inputs[0]), pin_type(node->inputs[1]));
            break;
        case GraphNode::Kind::unary: type = node->inputs.empty() ? "" : pin_type(node->inputs[0]); break;
        case GraphNode::Kind::select:
            type = node->inputs.size() == 3U ? widest(pin_type(node->inputs[1]), pin_type(node->inputs[2])) : "";
            break;
        case GraphNode::Kind::swizzle: type = swizzle_type(node->op); break;
        case GraphNode::Kind::set: type = node->inputs.empty() ? "" : pin_type(node->inputs[0]); break;
        case GraphNode::Kind::call:
            if (constructors.contains(node->op)) {
                type = node->op;
            } else if (const auto found = signatures.find(node->op); found != signatures.end()) {
                type = found->second;
            } else if (const auto* function = find_graph_function(node->op, node->inputs.size())) {
                if (function->result == "same") {
                    for (const auto& pin : node->inputs) type = widest(type, pin_type(pin));
                    if (node->op == "step" || node->op == "smoothstep") type = pin_type(node->inputs.back());
                } else {
                    type = std::string{function->result};
                }
            }
            break;
        case GraphNode::Kind::output: break;
        }
        node->type = type;
        if (type.empty() && node->kind != GraphNode::Kind::output) unknown.push_back(id);
        return type;
    };
    for (const auto& node : stage.nodes) (void)type_of(node.id);
    return unknown;
}

bool graph_would_cycle(const GraphStage& stage, const std::string_view from, const std::string_view to) {
    // A loop exists if `to` already feeds `from`.
    std::vector<std::string> pending{std::string{from}};
    std::set<std::string, std::less<>> seen;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (id == to) return true;
        if (!seen.insert(id).second) continue;
        if (const auto* node = stage.find(id))
            for (const auto& pin : node->inputs)
                if (!pin.source.empty()) pending.push_back(pin.source);
    }
    return false;
}

void layout_graph_stage(GraphStage& stage) {
    std::map<std::string, int, std::less<>> depth;
    std::function<int(const std::string&, int)> measure = [&](const std::string& id, const int guard) -> int {
        if (const auto found = depth.find(id); found != depth.end()) return found->second;
        const auto* node = stage.find(id);
        if (!node || guard > 256) return 0;
        int deepest = -1;
        for (const auto& pin : node->inputs)
            if (!pin.source.empty()) deepest = std::max(deepest, measure(pin.source, guard + 1));
        return depth[id] = deepest + 1;
    };
    int last = 0;
    for (const auto& node : stage.nodes)
        if (node.kind != GraphNode::Kind::output) last = std::max(last, measure(node.id, 0));
    std::map<int, int> rows;
    for (auto& node : stage.nodes) {
        const int column = node.kind == GraphNode::Kind::output ? last + 1 : measure(node.id, 0);
        node.x = static_cast<float>(column) * 215.0F;
        node.y = static_cast<float>(rows[column]++) * 100.0F;
    }
}

ShaderGraph shader_to_graph(const std::string_view text) {
    ShaderGraph graph;
    const auto parsed = parse_relay_shader(text);
    graph.type = parsed.type;
    graph.transparent = parsed.transparent;
    graph.unshaded = parsed.unshaded;
    graph.uniforms = parsed.uniforms;
    graph.errors = parsed.errors;
    // Node positions from the layout comment, if the file has one.
    std::map<std::string, std::map<std::string, std::pair<float, float>, std::less<>>, std::less<>> positions;
    std::size_t layout_start = std::string_view::npos;
    for (std::size_t at = text.find(layout_marker); at != std::string_view::npos;
         at = text.find(layout_marker, at + 1U))
        if (at == 0U || text[at - 1U] == '\n') layout_start = at;
    if (layout_start != std::string_view::npos) {
        const auto end = text.find('\n', layout_start);
        const auto json = text.substr(layout_start + layout_marker.size(),
                                      end == std::string_view::npos ? std::string_view::npos
                                                                    : end - layout_start - layout_marker.size());
        JsonParser parser(json);
        if (const auto value = parser.parse(); value && value->object())
            for (const auto& [stage_name, nodes] : *value->object())
                if (nodes.object())
                    for (const auto& [id, point] : *nodes.object())
                        if (const auto* pair = point.array();
                            pair && pair->size() == 2U && (*pair)[0].number() && (*pair)[1].number())
                            positions[stage_name][id] = {static_cast<float>(*(*pair)[0].number()),
                                                         static_cast<float>(*(*pair)[1].number())};
    }
    // Everything that is not a stage function (or the layout) is the graph's functions.
    auto functions = parsed.code;
    std::string masked = std::string{text};
    for (const auto& function : parsed.functions) {
        if (function.name != "vertex" && function.name != "fragment") continue;
        for (std::size_t index = function.begin; index < function.end && index < functions.size(); ++index)
            if (functions[index] != '\n') functions[index] = ' ';
    }
    if (layout_start != std::string_view::npos && layout_start < functions.size()) {
        const auto end = functions.find('\n', layout_start);
        for (std::size_t index = layout_start; index < (end == std::string::npos ? functions.size() : end); ++index)
            functions[index] = ' ';
    }
    graph.functions = trim_blank_lines(functions);
    if (!graph.errors.empty()) return graph;

    for (auto* stage : {&graph.vertex, &graph.fragment}) {
        if (graph.type == ShaderType::post_process && stage->name == "vertex") continue;
        const auto function = std::find_if(parsed.functions.begin(), parsed.functions.end(),
                                           [&](const ShaderFunction& item) { return item.name == stage->name; });
        stage->present = function != parsed.functions.end();
        std::string body;
        if (stage->present) {
            const auto open = text.find('{', function->begin);
            body = std::string{text.substr(open + 1U, function->end - 1U - open - 1U)};
        }
        // Comments are dropped for the nodes but kept when the stage stays code.
        std::string masked_body = body;
        for (std::size_t index = 0; index < masked_body.size(); ++index) {
            if (masked_body.compare(index, 2, "//") == 0) {
                while (index < masked_body.size() && masked_body[index] != '\n') masked_body[index++] = ' ';
            } else if (masked_body.compare(index, 2, "/*") == 0) {
                const auto end = masked_body.find("*/", index + 2U);
                const auto stop = end == std::string::npos ? masked_body.size() : end + 2U;
                for (; index < stop; ++index)
                    if (masked_body[index] != '\n') masked_body[index] = ' ';
            }
        }
        StageBuilder builder(graph, *stage);
        std::string reason;
        if (!builder.build(masked_body, reason)) {
            stage->nodes.clear();
            stage->code_only = true;
            stage->code = body;
            stage->reason = reason;
            continue;
        }
        (void)update_graph_types(graph, *stage);
        // Placed nodes keep their positions; new ones go where the automatic layout puts them.
        const auto placed = positions.find(stage->name);
        bool missing = false;
        for (const auto& node : stage->nodes)
            if (placed == positions.end() || !placed->second.contains(node.id)) missing = true;
        if (missing) layout_graph_stage(*stage);
        if (placed != positions.end())
            for (auto& node : stage->nodes)
                if (const auto found = placed->second.find(node.id); found != placed->second.end()) {
                    node.x = found->second.first;
                    node.y = found->second.second;
                }
    }
    return graph;
}

GraphSource graph_to_shader(const ShaderGraph& graph) {
    GraphSource source;
    std::vector<std::string> lines;
    lines.push_back("shader_type " + std::string{shader_type_name(graph.type)} + ';');
    std::vector<std::string> modes;
    if (graph.transparent) modes.emplace_back("transparent");
    if (graph.unshaded) modes.emplace_back("unshaded");
    if (!modes.empty()) {
        std::string line = "render_mode ";
        for (std::size_t index = 0; index < modes.size(); ++index) line += (index ? ", " : "") + modes[index];
        lines.push_back(line + ';');
    }
    if (!graph.uniforms.empty()) {
        lines.emplace_back();
        for (const auto& uniform : graph.uniforms) lines.push_back(uniform_declaration(uniform));
    }
    if (!graph.functions.empty()) {
        lines.emplace_back();
        std::istringstream text{graph.functions};
        std::string line;
        while (std::getline(text, line)) lines.push_back(line);
    }
    std::string layout = "{";
    for (const auto* stage : {&graph.vertex, &graph.fragment}) {
        const auto* output = stage->find("output");
        const bool assigns = output && std::any_of(output->inputs.begin(), output->inputs.end(), [](const GraphInput& pin) {
            return !pin.source.empty() || !pin.value.empty();
        });
        const bool nodes = std::any_of(stage->nodes.begin(), stage->nodes.end(),
                                       [](const GraphNode& node) { return node.kind != GraphNode::Kind::output; });
        if (!stage->present && !assigns && !nodes && !stage->code_only) continue;
        lines.emplace_back();
        lines.push_back("void " + stage->name + "() {");
        if (stage->code_only) {
            std::istringstream text{stage->code};
            std::string line;
            bool first = true;
            while (std::getline(text, line)) {
                // The body's first line is what followed the brace; drop it when empty.
                if (first && line.find_first_not_of(" \t\r") == std::string::npos) {
                    first = false;
                    continue;
                }
                first = false;
                lines.push_back(line);
            }
            while (lines.back().find_first_not_of(" \t\r") == std::string::npos) lines.pop_back();
            lines.push_back("}");
            continue;
        }
        // References: inputs by their built-in, parameters by their uniform, others by their id.
        std::set<std::string, std::less<>> inline_nodes;
        std::function<std::string(const GraphInput&, bool)> reference;
        std::function<std::string(const GraphNode&)> expression;
        const auto wrap = [](const std::string& text, const bool operand) {
            return operand && text.find_first_of(" ?") != std::string::npos ? '(' + text + ')' : text;
        };
        reference = [&](const GraphInput& pin, const bool operand) -> std::string {
            if (pin.source.empty()) return wrap(pin.value.empty() ? std::string{"0.0"} : pin.value, operand);
            const auto* node = stage->find(pin.source);
            if (!node) return "0.0";
            if (node->kind == GraphNode::Kind::input || node->kind == GraphNode::Kind::parameter) return node->op;
            if (inline_nodes.contains(node->id)) return wrap(expression(*node), operand);
            return node->id;
        };
        expression = [&](const GraphNode& node) -> std::string {
            const auto pin = [&](const std::size_t index, const bool operand) {
                return index < node.inputs.size() ? reference(node.inputs[index], operand) : std::string{"0.0"};
            };
            switch (node.kind) {
            case GraphNode::Kind::binary: return pin(0, true) + ' ' + node.op + ' ' + pin(1, true);
            case GraphNode::Kind::unary: return node.op + pin(0, true);
            case GraphNode::Kind::select: return pin(0, true) + " ? " + pin(1, true) + " : " + pin(2, true);
            case GraphNode::Kind::swizzle: return pin(0, true) + '.' + node.op;
            case GraphNode::Kind::call: {
                std::string text = node.op + '(';
                for (std::size_t index = 0; index < node.inputs.size(); ++index) text += (index ? ", " : "") + pin(index, false);
                return text + ')';
            }
            default: return pin(0, false);
            }
        };
        // Nodes whose type is unknown cannot be declared, so they are written where they are used.
        for (const auto& node : stage->nodes)
            if (node.type.empty() && node.kind != GraphNode::Kind::set && node.kind != GraphNode::Kind::output &&
                node.kind != GraphNode::Kind::input && node.kind != GraphNode::Kind::parameter)
                inline_nodes.insert(node.id);
        std::set<std::string, std::less<>> written;
        std::function<void(const GraphNode&, int)> write = [&](const GraphNode& node, const int guard) {
            if (written.contains(node.id) || guard > 512) return;
            written.insert(node.id);
            for (const auto& pin : node.inputs)
                if (const auto* input = pin.source.empty() ? nullptr : stage->find(pin.source)) write(*input, guard + 1);
            if (node.kind == GraphNode::Kind::input || node.kind == GraphNode::Kind::parameter ||
                node.kind == GraphNode::Kind::output || inline_nodes.contains(node.id))
                return;
            const auto line = static_cast<std::uint32_t>(lines.size() + 1U);
            if (node.kind == GraphNode::Kind::set) {
                const auto type = node.type.empty() ? std::string{"vec4"} : node.type;
                lines.push_back("    " + type + ' ' + node.id + " = " +
                                (node.inputs.empty() ? std::string{"0.0"} : reference(node.inputs[0], false)) + ';');
                lines.push_back("    " + node.id + '.' + node.op + " = " +
                                (node.inputs.size() > 1U ? reference(node.inputs[1], false) : std::string{"0.0"}) + ';');
                source.node_lines[line + 1U] = {stage->name, node.id};
            } else {
                lines.push_back("    " + node.type + ' ' + node.id + " = " + expression(node) + ';');
            }
            source.node_lines[line] = {stage->name, node.id};
        };
        for (const auto& node : stage->nodes) write(node, 0);
        if (output)
            for (const auto& pin : output->inputs)
                if (!pin.source.empty() || !pin.value.empty()) {
                    source.node_lines[static_cast<std::uint32_t>(lines.size() + 1U)] = {stage->name, "output"};
                    lines.push_back("    " + pin.name + " = " + reference(pin, false) + ';');
                }
        lines.push_back("}");
        layout += std::string(layout.size() > 1U ? "," : "") + '"' + stage->name + "\":{";
        // Sorted, so the same graph always writes the same text.
        std::map<std::string, std::pair<long long, long long>> placed;
        for (const auto& node : stage->nodes) placed[node.id] = {std::lround(node.x), std::lround(node.y)};
        bool first = true;
        for (const auto& [id, point] : placed) {
            layout += std::string(first ? "" : ",") + '"' + json_escape(id) + "\":[" + std::to_string(point.first) +
                      ',' + std::to_string(point.second) + ']';
            first = false;
        }
        layout += '}';
    }
    lines.emplace_back();
    lines.push_back(std::string{layout_marker} + layout + '}');
    for (const auto& line : lines) source.text += line + '\n';
    return source;
}

} // namespace relay
