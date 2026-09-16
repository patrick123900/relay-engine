#pragma once

// Native animation accessor decoding preserves glTF interpolation/tangents which Assimp drops.
// Geometry/material parsing remains in Assimp. Every external read uses its restricted IO handler.
#include "relay/core/json.hpp"
#include <bit>
#include <functional>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <tuple>

namespace relay::detail {
class GltfAnimationReader {
  public:
    using Read = std::function<bool(const std::string &, std::vector<std::uint8_t> &)>;
    GltfAnimationReader(std::string path, Read read)
        : path_(std::move(path)), read_(std::move(read)) {
        std::vector<std::uint8_t> bytes;
        require(read_(path_, bytes), "could not read animation document");
        std::string json;
        if (path_.ends_with(".glb")) {
            require(bytes.size() >= 20 && u32(bytes, 0) == 0x46546c67U && u32(bytes, 4) == 2 &&
                        u32(bytes, 8) == bytes.size(),
                    "invalid GLB");
            std::size_t offset = 12;
            while (offset + 8 <= bytes.size()) {
                const auto size = u32(bytes, offset), type = u32(bytes, offset + 4);
                offset += 8;
                require(size <= bytes.size() - offset, "invalid GLB chunk");
                if (type == 0x4e4f534aU)
                    json.assign(reinterpret_cast<const char *>(bytes.data() + offset), size);
                else if (type == 0x004e4942U)
                    binary_.assign(bytes.begin() + offset, bytes.begin() + offset + size);
                offset += size;
            }
            require(offset == bytes.size(), "truncated GLB chunk");
        } else
            json.assign(bytes.begin(), bytes.end());
        JsonParser parser{json};
        auto parsed = parser.parse();
        require(parsed && parsed->object(), "invalid glTF animation JSON");
        document_ = std::move(*parsed);
    }
    const JsonValue::Array &array(std::string_view key) const { return list(get(document_, key)); }
    const JsonValue &document() const { return document_; }
    static const JsonValue &get(const JsonValue &object, std::string_view key) {
        require(object.object(), "expected an animation object");
        const auto *value = field(*object.object(), key);
        require(value, "missing animation field");
        return *value;
    }
    static const JsonValue *optional_field(const JsonValue &object, std::string_view key) {
        require(object.object(), "expected an animation object");
        return field(*object.object(), key);
    }
    static std::size_t integer(const JsonValue &value) {
        require(value.number() && *value.number() >= 0 && *value.number() <= 1'000'000'000.0 &&
                    std::floor(*value.number()) == *value.number(),
                "invalid animation integer");
        return static_cast<std::size_t>(*value.number());
    }
    static std::size_t optional_integer(const JsonValue &value, std::string_view key,
                                        std::size_t fallback = 0) {
        require(value.object(), "expected object");
        const auto *v = field(*value.object(), key);
        return v ? integer(*v) : fallback;
    }
    static const JsonValue::Array &list(const JsonValue &value) {
        require(value.array(), "expected animation array");
        return *value.array();
    }
    static std::string string(const JsonValue &value) {
        require(value.string(), "expected animation string");
        return *value.string();
    }
    static const JsonValue &at(const JsonValue::Array &array, std::size_t i) {
        require(i < array.size(), "animation index outside array");
        return array[i];
    }
    std::vector<double> accessor(std::size_t index, unsigned components) {
        const auto &a = at(array("accessors"), index);
        require(integer(get(a, "componentType")) == 5126,
                "animation accessors must contain floats");
        const auto type = string(get(a, "type"));
        require(type == (components == 1   ? "SCALAR"
                         : components == 3 ? "VEC3"
                                           : "VEC4"),
                "animation accessor shape mismatch");
        const auto count = integer(get(a, "count"));
        require(count > 0 && count * components <= 4'000'000U,
                "animation accessor exceeds sample limit");
        total_samples_ += count * components;
        require(total_samples_ <= 8'000'000U, "animation payload exceeds sample limit");
        std::vector<double> output(count * components);
        const auto *view = optional_field(a, "bufferView");
        if (view)
            decode(integer(*view), optional_integer(a, "byteOffset"), count, components, output);
        const auto *sparse = optional_field(a, "sparse");
        if (sparse) {
            const auto size = integer(get(*sparse, "count"));
            require(size <= count, "sparse animation count mismatch");
            const auto &indices = get(*sparse, "indices");
            const auto &values = get(*sparse, "values");
            const auto kind = integer(get(indices, "componentType"));
            require(kind == 5121 || kind == 5123 || kind == 5125, "invalid sparse index type");
            const auto width = kind == 5121 ? 1U : kind == 5123 ? 2U : 4U;
            auto [index_bytes, index_start, index_length, index_stride] =
                view_bytes(integer(get(indices, "bufferView")));
            index_start += optional_integer(indices, "byteOffset");
            const auto index_offset = optional_integer(indices, "byteOffset");
            require(index_offset <= index_length && size * width <= index_length - index_offset,
                    "sparse indices exceed buffer view");
            std::vector<double> patches(size * components);
            decode(integer(get(values, "bufferView")), optional_integer(values, "byteOffset"), size,
                   components, patches);
            std::size_t previous = 0;
            for (std::size_t s = 0; s < size; ++s) {
                std::uint32_t vertex = 0;
                for (unsigned b = 0; b < width; ++b)
                    vertex |= std::uint32_t((*index_bytes)[index_start + s * width + b]) << (8 * b);
                require(vertex < count && (s == 0 || vertex > previous),
                        "invalid sparse animation index");
                previous = vertex;
                for (unsigned c = 0; c < components; ++c)
                    output[vertex * components + c] = patches[s * components + c];
            }
        }
        return output;
    }

  private:
    static void require(bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    static std::uint32_t u32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
        require(offset + 4 <= bytes.size(), "truncated animation buffer");
        return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
               (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
    }
    const std::vector<std::uint8_t> &buffer(std::size_t index) {
        if (const auto found = buffers_.find(index); found != buffers_.end())
            return found->second;
        const auto &source = at(array("buffers"), index);
        const auto *uri = optional_field(source, "uri");
        std::vector<std::uint8_t> bytes;
        if (!uri)
            bytes = binary_;
        else {
            const auto path = string(*uri);
            if (path.starts_with("data:")) {
                const auto comma = path.find(',');
                require(comma != std::string::npos && path.substr(0, comma).ends_with(";base64"),
                        "unsupported animation data URI");
                constexpr std::string_view alphabet =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::uint32_t value = 0;
                unsigned bits = 0;
                for (const char c : path.substr(comma + 1)) {
                    if (c == '=')
                        break;
                    const auto digit = alphabet.find(c);
                    require(digit != std::string_view::npos, "invalid animation base64");
                    value = (value << 6) | static_cast<std::uint32_t>(digit);
                    bits += 6;
                    if (bits >= 8) {
                        bits -= 8;
                        bytes.push_back(static_cast<std::uint8_t>(value >> bits));
                    }
                    require(bytes.size() <= 64U * 1024U * 1024U, "animation buffer exceeds 64 MiB");
                }
            } else
                require(read_((std::filesystem::path{path_}.parent_path() / path).generic_string(),
                              bytes),
                        "blocked or missing animation buffer");
        }
        require(integer(get(source, "byteLength")) <= bytes.size(),
                "animation buffer length mismatch");
        total_bytes_ += bytes.size();
        require(total_bytes_ <= 64U * 1024U * 1024U, "animation buffers exceed 64 MiB");
        return buffers_.emplace(index, std::move(bytes)).first->second;
    }
    using View =
        std::tuple<const std::vector<std::uint8_t> *, std::size_t, std::size_t, std::size_t>;
    View view_bytes(std::size_t index) {
        const auto &view = at(array("bufferViews"), index);
        const auto &bytes = buffer(integer(get(view, "buffer")));
        const auto start = optional_integer(view, "byteOffset"),
                   length = integer(get(view, "byteLength"));
        require(start <= bytes.size() && length <= bytes.size() - start,
                "animation view exceeds buffer");
        return {&bytes, start, length, optional_integer(view, "byteStride")};
    }
    void decode(std::size_t view, std::size_t offset, std::size_t count, unsigned components,
                std::vector<double> &output) {
        auto [bytes, start, length, stride] = view_bytes(view);
        if (!stride)
            stride = components * 4U;
        require(stride >= components * 4U && stride <= 252U && stride % 4U == 0U &&
                    offset <= length &&
                    (count == 0 || (count - 1) * stride + components * 4U <= length - offset),
                "animation accessor exceeds buffer view");
        for (std::size_t i = 0; i < count; ++i)
            for (unsigned c = 0; c < components; ++c) {
                const float value =
                    std::bit_cast<float>(u32(*bytes, start + offset + i * stride + c * 4U));
                require(std::isfinite(value), "nonfinite animation sample");
                output[i * components + c] = value;
            }
    }
    std::string path_;
    Read read_;
    JsonValue document_;
    std::vector<std::uint8_t> binary_;
    std::map<std::size_t, std::vector<std::uint8_t>> buffers_;
    std::size_t total_samples_{}, total_bytes_{};
};
} // namespace relay::detail
