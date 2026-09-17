#include "relay/control/generated_protocol.hpp"
#include "relay/core/json.hpp"
#include <algorithm>

#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <regex>
#include <variant>

namespace relay {
namespace {

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

bool validate_field(const ProtocolFieldSpec& spec, const JsonValue& scalar, std::string& error) {
    if (scalar.is_null()) {
        if (spec.nullable) return true;
        error = "field '" + std::string(spec.name) + "' cannot be null";
        return false;
    }
    if (spec.type == ProtocolValueType::string_array || spec.type == ProtocolValueType::number_array) {
        const auto* values = scalar.array();
        if (!values || values->size() < spec.minimum_length ||
            (spec.maximum_length && values->size() > spec.maximum_length)) {
            error = "field '" + std::string(spec.name) + "' must be an array of valid length";
            return false;
        }
        for (const auto& value : *values) {
            if (spec.type == ProtocolValueType::number_array) {
                if (!value.number() || !std::isfinite(*value.number())) {
                    error = "array entries must be finite numbers";
                    return false;
                }
            } else if (!value.string() || value.string()->size() > 32U ||
                       (!spec.pattern.empty() &&
                        !std::regex_match(*value.string(), std::regex(std::string(spec.pattern))))) {
                error = "array entries must be valid entity handles";
                return false;
            }
        }
        return true;
    }
    if (spec.type == ProtocolValueType::string) {
        const auto* value = scalar.string();
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
        if (scalar.boolean() == nullptr) {
            error = "field '" + std::string(spec.name) + "' must be boolean";
            return false;
        }
        return true;
    }
    const auto* value = scalar.number();
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
    JsonParser parser(request);
    const auto parsed = parser.parse();
    if (!parsed || !parsed->object()) {
        error = parser.error().find("duplicate") != std::string::npos
                    ? "request contains a duplicate field"
                    : "request must be a valid JSON object: " + parser.error();
        return false;
    }
    const auto& fields = *parsed->object();
    const auto id = fields.find("id");
    if (id == fields.end() || id->second.number() == nullptr ||
        std::floor(*id->second.number()) != *id->second.number() ||
        *id->second.number() < 0.0) {
        error = "request id must be a non-negative integer";
        return false;
    }
    const auto method_field = fields.find("method");
    if (method_field == fields.end() || method_field->second.string() == nullptr ||
        *method_field->second.string() != method) {
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
