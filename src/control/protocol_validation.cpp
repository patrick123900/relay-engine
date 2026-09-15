#include "relay/control/generated_protocol.hpp"

#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <regex>
#include <variant>

namespace relay {
namespace {

struct Scalar {
    std::variant<std::monostate, bool, double, std::string> value;
};

class ObjectParser {
public:
    explicit ObjectParser(const std::string_view text) : text_(text) {}

    [[nodiscard]] bool parse(std::map<std::string, Scalar, std::less<>>& fields,
                             std::string& error) {
        skip_space();
        if (!take('{')) return fail(error, "request must be a JSON object");
        skip_space();
        if (take('}')) return finish(error);
        while (position_ < text_.size()) {
            auto key = parse_string(error);
            if (!key) return false;
            skip_space();
            if (!take(':')) return fail(error, "expected ':' after request field");
            skip_space();
            auto value = parse_scalar(error);
            if (!value) return false;
            if (!fields.emplace(std::move(*key), std::move(*value)).second) {
                return fail(error, "request contains a duplicate field");
            }
            skip_space();
            if (take('}')) return finish(error);
            if (!take(',')) return fail(error, "expected ',' between request fields");
            skip_space();
        }
        return fail(error, "unterminated request object");
    }

private:
    void skip_space() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
                                            text_[position_] == '\r' || text_[position_] == '\n')) {
            ++position_;
        }
    }

    bool take(const char expected) {
        if (position_ >= text_.size() || text_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool finish(std::string& error) {
        skip_space();
        return position_ == text_.size() || fail(error, "unexpected content after request object");
    }

    bool fail(std::string& error, const std::string_view message) const {
        error = std::string(message) + " at byte " + std::to_string(position_);
        return false;
    }

    [[nodiscard]] std::optional<std::string> parse_string(std::string& error) {
        if (!take('"')) {
            fail(error, "expected a JSON string");
            return std::nullopt;
        }
        std::string result;
        while (position_ < text_.size()) {
            const auto character = static_cast<unsigned char>(text_[position_++]);
            if (character == '"') return result;
            if (character < 0x20U) {
                fail(error, "unescaped control character in string");
                return std::nullopt;
            }
            if (character != '\\') {
                result += static_cast<char>(character);
                continue;
            }
            if (position_ >= text_.size()) {
                fail(error, "incomplete string escape");
                return std::nullopt;
            }
            switch (text_[position_++]) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default:
                fail(error, "unsupported string escape in control request");
                return std::nullopt;
            }
        }
        fail(error, "unterminated string");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Scalar> parse_scalar(std::string& error) {
        if (position_ >= text_.size()) {
            fail(error, "missing request value");
            return std::nullopt;
        }
        if (text_[position_] == '"') {
            auto string = parse_string(error);
            return string ? std::optional{Scalar{std::move(*string)}} : std::nullopt;
        }
        for (const auto& literal : {std::pair{"true", Scalar{true}},
                                    std::pair{"false", Scalar{false}},
                                    std::pair{"null", Scalar{}}}) {
            const std::string_view name = literal.first;
            if (text_.substr(position_, name.size()) == name) {
                position_ += name.size();
                return literal.second;
            }
        }
        const auto start = position_;
        while (position_ < text_.size() && ((text_[position_] >= '0' && text_[position_] <= '9') ||
                                             text_[position_] == '-' || text_[position_] == '+' ||
                                             text_[position_] == '.' || text_[position_] == 'e' ||
                                             text_[position_] == 'E')) ++position_;
        double number = 0.0;
        const auto converted = std::from_chars(text_.data() + start, text_.data() + position_, number);
        if (start == position_ || converted.ec != std::errc{} ||
            converted.ptr != text_.data() + position_ || !std::isfinite(number)) {
            fail(error, "request value must be a finite scalar");
            return std::nullopt;
        }
        return Scalar{number};
    }

    std::string_view text_;
    std::size_t position_{};
};

bool enum_contains(const std::string_view values, const std::string_view candidate) {
    if (values.empty()) return true;
    std::size_t start = 0;
    while (start <= values.size()) {
        const auto end = values.find('|', start);
        const auto value = values.substr(start, end == std::string_view::npos ? values.size() - start
                                                                              : end - start);
        if (value == candidate) return true;
        if (end == std::string_view::npos) break;
        start = end + 1U;
    }
    return false;
}

bool validate_field(const ProtocolFieldSpec& spec, const Scalar& scalar, std::string& error) {
    if (std::holds_alternative<std::monostate>(scalar.value)) {
        if (spec.nullable) return true;
        error = "field '" + std::string(spec.name) + "' cannot be null";
        return false;
    }
    if (spec.type == ProtocolValueType::string) {
        const auto* value = std::get_if<std::string>(&scalar.value);
        if (value == nullptr) {
            error = "field '" + std::string(spec.name) + "' must be a string";
            return false;
        }
        if ((spec.minimum_length != 0U && value->size() < spec.minimum_length) ||
            (spec.maximum_length != 0U && value->size() > spec.maximum_length)) {
            error = "field '" + std::string(spec.name) + "' has an invalid length";
            return false;
        }
        if (!spec.pattern.empty() && !std::regex_match(*value, std::regex(std::string(spec.pattern)))) {
            error = "field '" + std::string(spec.name) + "' has an invalid format";
            return false;
        }
        if (!enum_contains(spec.enum_values, *value)) {
            error = "field '" + std::string(spec.name) + "' is not an allowed value";
            return false;
        }
        return true;
    }
    if (spec.type == ProtocolValueType::boolean) {
        if (!std::holds_alternative<bool>(scalar.value)) {
            error = "field '" + std::string(spec.name) + "' must be boolean";
            return false;
        }
        return true;
    }
    const auto* value = std::get_if<double>(&scalar.value);
    if (value == nullptr || (spec.type == ProtocolValueType::integer && std::floor(*value) != *value)) {
        error = "field '" + std::string(spec.name) + "' must be " +
                (spec.type == ProtocolValueType::integer ? "integer" : "number");
        return false;
    }
    if ((spec.has_minimum && *value < spec.minimum) || (spec.has_maximum && *value > spec.maximum)) {
        error = "field '" + std::string(spec.name) + "' is outside its allowed range";
        return false;
    }
    return true;
}

} // namespace

bool validate_protocol_request(const std::string_view request, const std::string_view method,
                               std::string& error) {
    const auto* method_spec = find_protocol_method(method);
    if (method_spec == nullptr) {
        error = "unknown method: " + std::string(method);
        return false;
    }
    std::map<std::string, Scalar, std::less<>> fields;
    ObjectParser parser(request);
    if (!parser.parse(fields, error)) return false;
    const auto id = fields.find("id");
    if (id == fields.end() || std::get_if<double>(&id->second.value) == nullptr ||
        std::floor(*std::get_if<double>(&id->second.value)) != *std::get_if<double>(&id->second.value) ||
        *std::get_if<double>(&id->second.value) < 0.0) {
        error = "request id must be a non-negative integer";
        return false;
    }
    const auto method_field = fields.find("method");
    if (method_field == fields.end() || std::get_if<std::string>(&method_field->second.value) == nullptr ||
        *std::get_if<std::string>(&method_field->second.value) != method) {
        error = "request method is invalid";
        return false;
    }
    for (const auto& spec : method_spec->fields) {
        const auto field = fields.find(spec.name);
        if (field == fields.end()) {
            if (spec.required) {
                error = "missing required field '" + std::string(spec.name) + "'";
                return false;
            }
            continue;
        }
        if (!validate_field(spec, field->second, error)) return false;
    }
    for (const auto& [name, value] : fields) {
        (void)value;
        if (name == "id" || name == "method") continue;
        const auto known = std::any_of(method_spec->fields.begin(), method_spec->fields.end(),
                                       [&name](const auto& spec) { return spec.name == name; });
        if (!known) {
            error = "unknown field '" + name + "' for method '" + std::string(method) + "'";
            return false;
        }
    }
    return true;
}

} // namespace relay
