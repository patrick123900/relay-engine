#pragma once

// Shared strict JSON reader. Relay parses its own on-disk formats (scene files, the import
// manifest) with one implementation so validation rules stay identical across them.

#include <charconv>
#include <limits>
#include <concepts>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <iomanip>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace relay {

inline constexpr std::size_t max_json_depth = 64U;

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    std::variant<std::monostate, bool, double, std::string, Array, Object> data;

    [[nodiscard]] const Object* object() const { return std::get_if<Object>(&data); }
    [[nodiscard]] const Array* array() const { return std::get_if<Array>(&data); }
    [[nodiscard]] const std::string* string() const { return std::get_if<std::string>(&data); }
    [[nodiscard]] const double* number() const { return std::get_if<double>(&data); }
    [[nodiscard]] const bool* boolean() const { return std::get_if<bool>(&data); }
    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::monostate>(data); }
};

class JsonParser {
public:
    explicit JsonParser(const std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<JsonValue> parse() {
        JsonValue value;
        const bool parsed = parse_value(value, 0);
        skip_space();
        if (parsed && position_ != text_.size()) fail("unexpected trailing content");
        if (!parsed || !error_.empty()) return std::nullopt;
        return value;
    }

    [[nodiscard]] std::string error() const {
        return error_.empty() ? std::string{} : error_ + " at byte " + std::to_string(position_);
    }

private:
    void skip_space() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\n' ||
                                            text_[position_] == '\r' || text_[position_] == '\t')) {
            ++position_;
        }
    }

    void fail(const std::string_view message) {
        if (error_.empty()) error_ = message;
    }

    // Values are parsed in place into their final home; building each one and moving it through
    // several wrappers dominated the cost, most of all in unoptimized builds.
    [[nodiscard]] bool parse_value(JsonValue& out, const std::size_t depth) {
        skip_space();
        if (depth > max_json_depth) {
            fail("JSON nesting limit exceeded");
            return false;
        }
        if (position_ >= text_.size()) {
            fail("unexpected end of JSON");
            return false;
        }
        switch (text_[position_]) {
        case '{': return parse_object(out, depth + 1U);
        case '[': return parse_array(out, depth + 1U);
        case '"': {
            auto& text = out.data.emplace<std::string>();
            return parse_string(text);
        }
        case 't': return parse_literal("true") && (out.data = true, true);
        case 'f': return parse_literal("false") && (out.data = false, true);
        case 'n': return parse_literal("null") && (out.data = std::monostate{}, true);
        default: return parse_number(out);
        }
    }

    [[nodiscard]] bool parse_literal(const std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) {
            fail("invalid JSON value");
            return false;
        }
        position_ += literal.size();
        return true;
    }

    static void append_utf8(std::string& output, const std::uint32_t codepoint) {
        if (codepoint <= 0x7FU) output += static_cast<char>(codepoint);
        else if (codepoint <= 0x7FFU) {
            output += static_cast<char>(0xC0U | (codepoint >> 6U));
            output += static_cast<char>(0x80U | (codepoint & 0x3FU));
        } else if (codepoint <= 0xFFFFU) {
            output += static_cast<char>(0xE0U | (codepoint >> 12U));
            output += static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
            output += static_cast<char>(0x80U | (codepoint & 0x3FU));
        } else {
            output += static_cast<char>(0xF0U | (codepoint >> 18U));
            output += static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU));
            output += static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
            output += static_cast<char>(0x80U | (codepoint & 0x3FU));
        }
    }

    [[nodiscard]] std::optional<std::uint32_t> parse_hex_quad() {
        if (position_ + 4U > text_.size()) {
            fail("incomplete Unicode escape");
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::size_t offset = 0; offset < 4U; ++offset) {
            const char character = text_[position_++];
            value <<= 4U;
            if (character >= '0' && character <= '9') value |= static_cast<std::uint32_t>(character - '0');
            else if (character >= 'a' && character <= 'f') value |= static_cast<std::uint32_t>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F') value |= static_cast<std::uint32_t>(character - 'A' + 10);
            else {
                fail("invalid Unicode escape");
                return std::nullopt;
            }
        }
        return value;
    }

    [[nodiscard]] bool parse_string(std::string& result) {
        if (text_[position_++] != '"') return false;
        while (position_ < text_.size()) {
            // Plain runs are appended at once; most strings have no escapes at all.
            const auto run_start = position_;
            while (position_ < text_.size()) {
                const auto plain = static_cast<unsigned char>(text_[position_]);
                if (plain == '"' || plain == '\\' || plain < 0x20U) break;
                ++position_;
            }
            if (position_ > run_start) result.append(text_.data() + run_start, position_ - run_start);
            if (position_ >= text_.size()) break;
            const unsigned char character = static_cast<unsigned char>(text_[position_++]);
            if (character == '"') return true;
            if (character < 0x20U) {
                fail("unescaped control character in string");
                return false;
            }
            if (character != '\\') {
                result += static_cast<char>(character);
                continue;
            }
            if (position_ >= text_.size()) {
                fail("incomplete string escape");
                return false;
            }
            const char escaped = text_[position_++];
            switch (escaped) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                auto first = parse_hex_quad();
                if (!first) return false;
                std::uint32_t codepoint = *first;
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (position_ + 2U > text_.size() || text_[position_] != '\\' ||
                        text_[position_ + 1U] != 'u') {
                        fail("high surrogate without a low surrogate");
                        return false;
                    }
                    position_ += 2U;
                    auto second = parse_hex_quad();
                    if (!second || *second < 0xDC00U || *second > 0xDFFFU) {
                        fail("invalid low surrogate");
                        return false;
                    }
                    codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (*second - 0xDC00U);
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    fail("unexpected low surrogate");
                    return false;
                }
                append_utf8(result, codepoint);
                break;
            }
            default:
                fail("invalid string escape");
                return false;
            }
        }
        fail("unterminated string");
        return false;
    }

    [[nodiscard]] bool parse_number(JsonValue& out) {
        const auto start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) {
            fail("invalid number");
            return false;
        }
        if (text_[position_] == '0') ++position_;
        else if (text_[position_] >= '1' && text_[position_] <= '9') {
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        } else {
            fail("invalid number");
            return false;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            const auto digits = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (digits == position_) {
                fail("invalid number fraction");
                return false;
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            const auto digits = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (digits == position_) {
                fail("invalid number exponent");
                return false;
            }
        }
        double number = 0.0;
        const auto converted = std::from_chars(text_.data() + start, text_.data() + position_, number);
        if (converted.ec != std::errc{} || converted.ptr != text_.data() + position_ || !std::isfinite(number)) {
            fail("number is not finite or representable");
            return false;
        }
        out.data = number;
        return true;
    }

    [[nodiscard]] bool parse_array(JsonValue& out, const std::size_t depth) {
        ++position_;
        auto& result = out.data.emplace<JsonValue::Array>();
        skip_space();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return true;
        }
        result.reserve(4); // Most arrays are short vectors; this skips their early regrowth.
        while (true) {
            if (!parse_value(result.emplace_back(), depth)) return false;
            skip_space();
            if (position_ >= text_.size()) break;
            const char separator = text_[position_++];
            if (separator == ']') return true;
            if (separator != ',') break;
        }
        fail("unterminated JSON array");
        return false;
    }

    [[nodiscard]] bool parse_object(JsonValue& out, const std::size_t depth) {
        ++position_;
        auto& result = out.data.emplace<JsonValue::Object>();
        skip_space();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return true;
        }
        std::string key;
        while (true) {
            skip_space();
            if (position_ >= text_.size() || text_[position_] != '"') {
                fail("object key must be a string");
                return false;
            }
            key.clear();
            if (!parse_string(key)) return false;
            skip_space();
            if (position_ >= text_.size() || text_[position_++] != ':') {
                fail("missing colon after object key");
                return false;
            }
            auto [member, inserted] = result.try_emplace(std::move(key));
            if (!inserted) {
                fail("duplicate object key");
                return false;
            }
            if (!parse_value(member->second, depth)) return false;
            skip_space();
            if (position_ >= text_.size()) break;
            const char separator = text_[position_++];
            if (separator == '}') return true;
            if (separator != ',') break;
        }
        fail("unterminated JSON object");
        return false;
    }

    std::string_view text_;
    std::size_t position_{0};
    std::string error_;
};

// Builds JSON text with the `<<` syntax of an ostringstream but without its per-insertion cost, which
// dominated large listings such as the scene and collider overlays the editor polls. Numbers print
// as a stream would with default float formatting: 17 significant digits unless `precision` says
// otherwise.
class JsonWriter {
public:
    JsonWriter& operator<<(const char* text) { text_ += text; return *this; }
    JsonWriter& operator<<(const char character) { text_ += character; return *this; }
    JsonWriter& operator<<(const std::string_view text) { text_ += text; return *this; }
    JsonWriter& operator<<(const std::string& text) { text_ += text; return *this; }
    JsonWriter& operator<<(const double value) {
        char buffer[40];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                                          precision_);
        text_.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
        return *this;
    }
    JsonWriter& operator<<(const float value) { return *this << static_cast<double>(value); }
    template <std::integral Integer>
    JsonWriter& operator<<(const Integer value) {
        if constexpr (std::same_as<Integer, bool>) {
            text_ += value ? '1' : '0';
        } else {
            char buffer[24];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            text_.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
        }
        return *this;
    }
    void precision(const int digits) { precision_ = digits; }
    [[nodiscard]] std::string str() { return std::move(text_); }
    [[nodiscard]] const std::string& view() const { return text_; }

private:
    std::string text_;
    int precision_{17};
};

// Escapes a value for embedding in Relay's hand-written JSON output.
inline std::string json_escape(const std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                constexpr char hex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += hex[static_cast<unsigned char>(character) >> 4U];
                escaped += hex[static_cast<unsigned char>(character) & 15U];
            } else
                escaped += character;
            break;
        }
    }
    return escaped;
}

inline std::string json_stringify(const JsonValue &value) {
    if (value.is_null())
        return "null";
    if (const auto *b = value.boolean())
        return *b ? "true" : "false";
    if (const auto *n = value.number()) {
        std::ostringstream stream;
        stream << std::setprecision(17) << *n;
        return stream.str();
    }
    if (const auto *s = value.string())
        return "\"" + json_escape(*s) + "\"";
    std::string output;
    if (const auto *a = value.array()) {
        output = "[";
        for (std::size_t i = 0; i < a->size(); ++i) {
            if (i)
                output += ',';
            output += json_stringify((*a)[i]);
        }
        return output + "]";
    }
    output = "{";
    bool first = true;
    for (const auto &[key, v] : *value.object()) {
        if (!first)
            output += ',';
        first = false;
        output += "\"" + json_escape(key) + "\":" + json_stringify(v);
    }
    return output + "}";
}

inline const JsonValue* field(const JsonValue::Object& object, const std::string_view name) {
    const auto iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

} // namespace relay
