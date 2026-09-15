#include "relay/scene/scene_io.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <string_view>
#include <variant>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace relay {
namespace {

constexpr std::uintmax_t max_scene_bytes = 16U * 1024U * 1024U;
constexpr std::size_t max_entities = 100'000U;
constexpr std::uint32_t max_entity_index = 1'000'000U;
constexpr std::size_t max_name_bytes = 4096U;
constexpr std::size_t max_json_depth = 64U;

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
        auto value = parse_value(0);
        skip_space();
        if (value && position_ != text_.size()) fail("unexpected trailing content");
        return error_.empty() ? value : std::nullopt;
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

    [[nodiscard]] std::optional<JsonValue> parse_value(const std::size_t depth) {
        skip_space();
        if (depth > max_json_depth) {
            fail("JSON nesting limit exceeded");
            return std::nullopt;
        }
        if (position_ >= text_.size()) {
            fail("unexpected end of JSON");
            return std::nullopt;
        }
        switch (text_[position_]) {
        case '{': return parse_object(depth + 1U);
        case '[': return parse_array(depth + 1U);
        case '"': {
            auto value = parse_string();
            if (!value) return std::nullopt;
            return JsonValue{std::move(*value)};
        }
        case 't': return parse_literal("true", JsonValue{true});
        case 'f': return parse_literal("false", JsonValue{false});
        case 'n': return parse_literal("null", JsonValue{});
        default: return parse_number();
        }
    }

    [[nodiscard]] std::optional<JsonValue> parse_literal(const std::string_view literal,
                                                         JsonValue value) {
        if (text_.substr(position_, literal.size()) != literal) {
            fail("invalid JSON value");
            return std::nullopt;
        }
        position_ += literal.size();
        return value;
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

    [[nodiscard]] std::optional<std::string> parse_string() {
        if (text_[position_++] != '"') return std::nullopt;
        std::string result;
        while (position_ < text_.size()) {
            const unsigned char character = static_cast<unsigned char>(text_[position_++]);
            if (character == '"') return result;
            if (character < 0x20U) {
                fail("unescaped control character in string");
                return std::nullopt;
            }
            if (character != '\\') {
                result += static_cast<char>(character);
                continue;
            }
            if (position_ >= text_.size()) {
                fail("incomplete string escape");
                return std::nullopt;
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
                if (!first) return std::nullopt;
                std::uint32_t codepoint = *first;
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (position_ + 2U > text_.size() || text_[position_] != '\\' ||
                        text_[position_ + 1U] != 'u') {
                        fail("high surrogate without a low surrogate");
                        return std::nullopt;
                    }
                    position_ += 2U;
                    auto second = parse_hex_quad();
                    if (!second || *second < 0xDC00U || *second > 0xDFFFU) {
                        fail("invalid low surrogate");
                        return std::nullopt;
                    }
                    codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (*second - 0xDC00U);
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    fail("unexpected low surrogate");
                    return std::nullopt;
                }
                append_utf8(result, codepoint);
                break;
            }
            default:
                fail("invalid string escape");
                return std::nullopt;
            }
        }
        fail("unterminated string");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<JsonValue> parse_number() {
        const auto start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) {
            fail("invalid number");
            return std::nullopt;
        }
        if (text_[position_] == '0') ++position_;
        else if (text_[position_] >= '1' && text_[position_] <= '9') {
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        } else {
            fail("invalid number");
            return std::nullopt;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            const auto digits = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (digits == position_) {
                fail("invalid number fraction");
                return std::nullopt;
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            const auto digits = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (digits == position_) {
                fail("invalid number exponent");
                return std::nullopt;
            }
        }
        double number = 0.0;
        const auto converted = std::from_chars(text_.data() + start, text_.data() + position_, number);
        if (converted.ec != std::errc{} || converted.ptr != text_.data() + position_ || !std::isfinite(number)) {
            fail("number is not finite or representable");
            return std::nullopt;
        }
        return JsonValue{number};
    }

    [[nodiscard]] std::optional<JsonValue> parse_array(const std::size_t depth) {
        ++position_;
        JsonValue::Array result;
        skip_space();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return JsonValue{std::move(result)};
        }
        while (true) {
            auto value = parse_value(depth);
            if (!value) return std::nullopt;
            result.push_back(std::move(*value));
            skip_space();
            if (position_ >= text_.size()) break;
            const char separator = text_[position_++];
            if (separator == ']') return JsonValue{std::move(result)};
            if (separator != ',') break;
        }
        fail("unterminated JSON array");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<JsonValue> parse_object(const std::size_t depth) {
        ++position_;
        JsonValue::Object result;
        skip_space();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return JsonValue{std::move(result)};
        }
        while (true) {
            skip_space();
            if (position_ >= text_.size() || text_[position_] != '"') {
                fail("object key must be a string");
                return std::nullopt;
            }
            auto key = parse_string();
            if (!key) return std::nullopt;
            skip_space();
            if (position_ >= text_.size() || text_[position_++] != ':') {
                fail("missing colon after object key");
                return std::nullopt;
            }
            auto value = parse_value(depth);
            if (!value) return std::nullopt;
            if (!result.emplace(std::move(*key), std::move(*value)).second) {
                fail("duplicate object key");
                return std::nullopt;
            }
            skip_space();
            if (position_ >= text_.size()) break;
            const char separator = text_[position_++];
            if (separator == '}') return JsonValue{std::move(result)};
            if (separator != ',') break;
        }
        fail("unterminated JSON object");
        return std::nullopt;
    }

    std::string_view text_;
    std::size_t position_{0};
    std::string error_;
};

const JsonValue* field(const JsonValue::Object& object, const std::string_view name) {
    const auto iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

bool read_integer(const JsonValue* value, std::uint32_t& output) {
    if (value == nullptr || value->number() == nullptr) return false;
    const double number = *value->number();
    if (number < 0.0 || number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        std::floor(number) != number) return false;
    output = static_cast<std::uint32_t>(number);
    return true;
}

bool read_vec3(const JsonValue* value, Vec3& output) {
    if (value == nullptr || value->object() == nullptr) return false;
    const auto& object = *value->object();
    const auto* x = field(object, "x");
    const auto* y = field(object, "y");
    const auto* z = field(object, "z");
    if (x == nullptr || y == nullptr || z == nullptr || x->number() == nullptr ||
        y->number() == nullptr || z->number() == nullptr) return false;
    output = {*x->number(), *y->number(), *z->number()};
    return std::isfinite(output.x) && std::isfinite(output.y) && std::isfinite(output.z);
}

bool validate_component_metadata(const JsonValue* value, std::string& error) {
    if (value == nullptr || value->array() == nullptr) {
        error = "versioned scene requires a components array";
        return false;
    }
    for (const auto& component : *value->array()) {
        const auto* object = component.object();
        std::uint32_t stable_id = 0;
        if (object == nullptr || field(*object, "name") == nullptr ||
            field(*object, "name")->string() == nullptr ||
            !read_integer(field(*object, "stable_id"), stable_id) || stable_id == 0U) {
            error = "component metadata has an invalid name or stable_id";
            return false;
        }
        const auto* fields = field(*object, "fields");
        if (fields == nullptr || fields->array() == nullptr) {
            error = "component metadata fields must be an array";
            return false;
        }
        for (const auto& reflected_field : *fields->array()) {
            const auto* reflected_object = reflected_field.object();
            if (reflected_object == nullptr || field(*reflected_object, "name") == nullptr ||
                field(*reflected_object, "name")->string() == nullptr ||
                field(*reflected_object, "type") == nullptr ||
                field(*reflected_object, "type")->string() == nullptr) {
                error = "component field metadata is invalid";
                return false;
            }
        }
    }
    return true;
}

bool hierarchy_is_valid(const SceneState& state, std::string& error) {
    std::vector<unsigned char> marks(state.slots.size(), 0U);
    for (std::size_t start = 0; start < state.slots.size(); ++start) {
        if (!state.slots[start].alive || marks[start] == 2U) continue;
        std::vector<std::size_t> chain;
        auto current = start;
        while (true) {
            if (marks[current] == 1U) {
                error = "entity hierarchy contains a cycle";
                return false;
            }
            if (marks[current] == 2U) break;
            marks[current] = 1U;
            chain.push_back(current);
            const auto parent = state.slots[current].record.parent;
            if (!parent.valid()) break;
            if (parent.index >= state.slots.size() || !state.slots[parent.index].alive ||
                state.slots[parent.index].generation != parent.generation) {
                error = "entity references a missing or stale parent " + parent.to_string();
                return false;
            }
            current = parent.index;
        }
        for (const auto index : chain) marks[index] = 2U;
    }
    return true;
}

std::filesystem::path temporary_path_for(const std::filesystem::path& destination) {
    static std::atomic<std::uint64_t> serial{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return destination.parent_path() /
           (destination.filename().string() + ".tmp." + std::to_string(ticks) + '.' +
            std::to_string(serial.fetch_add(1U, std::memory_order_relaxed)));
}

bool replace_atomically(const std::filesystem::path& temporary,
                        const std::filesystem::path& destination, std::string& error) {
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "atomic scene replacement failed with Windows error " + std::to_string(GetLastError());
        return false;
    }
#else
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
        error = "atomic scene replacement failed";
        return false;
    }
#endif
    return true;
}

} // namespace

SceneFileLoadResult load_scene_file(const std::filesystem::path& path) {
    SceneFileLoadResult result;
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        result.error = "could not inspect scene file: " + filesystem_error.message();
        return result;
    }
    if (size > max_scene_bytes) {
        result.error = "scene file exceeds the 16 MiB limit";
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size())) && !text.empty()) {
        result.error = "could not read the complete scene file";
        return result;
    }

    JsonParser parser(text);
    auto document = parser.parse();
    if (!document) {
        result.error = parser.error();
        return result;
    }
    const auto* root = document->object();
    if (root == nullptr) {
        result.error = "scene document root must be an object";
        return result;
    }
    const auto* format = field(*root, "format");
    if (format == nullptr || format->string() == nullptr || *format->string() != "relay.scene") {
        result.error = "scene format must be relay.scene";
        return result;
    }
    if (!read_integer(field(*root, "version"), result.source_version) || result.source_version > scene_file_version) {
        result.error = "unsupported scene file version";
        return result;
    }
    result.migrated = result.source_version < scene_file_version;
    if (result.source_version >= 1U &&
        !validate_component_metadata(field(*root, "components"), result.error)) {
        return result;
    }

    const JsonValue* entities_value = nullptr;
    const JsonValue::Object* scene_object = nullptr;
    if (result.source_version == 0U) entities_value = field(*root, "entities");
    else {
        const auto* scene = field(*root, "scene");
        if (scene != nullptr) scene_object = scene->object();
        if (scene_object != nullptr) entities_value = field(*scene_object, "entities");
    }
    if (entities_value == nullptr || entities_value->array() == nullptr) {
        result.error = "scene entities must be an array";
        return result;
    }
    if (entities_value->array()->size() > max_entities) {
        result.error = "scene exceeds the 100000 entity limit";
        return result;
    }

    SceneState state;
    for (const auto& entity_value : *entities_value->array()) {
        const auto* entity_object = entity_value.object();
        if (entity_object == nullptr) {
            result.error = "every entity must be an object";
            return result;
        }
        const auto* encoded_entity = field(*entity_object, "entity");
        const auto parsed_entity = encoded_entity != nullptr && encoded_entity->string() != nullptr
                                       ? Entity::parse(*encoded_entity->string()) : std::nullopt;
        if (!parsed_entity || parsed_entity->index > max_entity_index) {
            result.error = "entity has an invalid or excessive handle";
            return result;
        }
        if (state.slots.size() <= parsed_entity->index) state.slots.resize(parsed_entity->index + 1U);
        auto& slot = state.slots[parsed_entity->index];
        if (slot.alive) {
            result.error = "scene contains a duplicate entity handle index";
            return result;
        }
        const auto* name = field(*entity_object, "name");
        if (name == nullptr || name->string() == nullptr || name->string()->size() > max_name_bytes) {
            result.error = "entity name is missing or exceeds 4096 bytes";
            return result;
        }
        Entity parent{};
        const auto* parent_value = field(*entity_object, "parent");
        if (parent_value == nullptr) {
            result.error = "entity parent is missing";
            return result;
        }
        if (!parent_value->is_null()) {
            if (parent_value->string() == nullptr) {
                result.error = "entity parent must be null or a handle string";
                return result;
            }
            const auto parsed_parent = Entity::parse(*parent_value->string());
            if (!parsed_parent) {
                result.error = "entity parent handle is invalid";
                return result;
            }
            parent = *parsed_parent;
        }
        const auto* transform_value = field(*entity_object, "transform");
        if (transform_value == nullptr || transform_value->object() == nullptr) {
            result.error = "entity transform must be an object";
            return result;
        }
        const auto& transform_object = *transform_value->object();
        Transform transform;
        const auto rotation_name = result.source_version == 0U ? "rotation" : "rotation_degrees";
        if (!read_vec3(field(transform_object, "position"), transform.position) ||
            !read_vec3(field(transform_object, rotation_name), transform.rotation_degrees) ||
            !read_vec3(field(transform_object, "scale"), transform.scale)) {
            result.error = "entity transform requires finite position, rotation and scale vectors";
            return result;
        }
        std::optional<Camera> camera;
        if (result.source_version >= 2U) {
            const auto* camera_value = field(*entity_object, "camera");
            if (camera_value == nullptr) {
                result.error = "version 2 entity camera field is missing";
                return result;
            }
            if (!camera_value->is_null()) {
                const auto* camera_object = camera_value->object();
                if (camera_object == nullptr) {
                    result.error = "entity camera must be null or an object";
                    return result;
                }
                const auto* fov = field(*camera_object, "field_of_view_y_degrees");
                const auto* near_plane = field(*camera_object, "near_plane");
                const auto* far_plane = field(*camera_object, "far_plane");
                const auto* active = field(*camera_object, "active");
                if (fov == nullptr || fov->number() == nullptr || near_plane == nullptr ||
                    near_plane->number() == nullptr || far_plane == nullptr ||
                    far_plane->number() == nullptr || active == nullptr || active->boolean() == nullptr) {
                    result.error = "camera requires numeric projection fields and an active flag";
                    return result;
                }
                camera = Camera{*fov->number(), *near_plane->number(), *far_plane->number(),
                                *active->boolean()};
                if (!std::isfinite(camera->field_of_view_y_degrees) ||
                    camera->field_of_view_y_degrees <= 1.0 ||
                    camera->field_of_view_y_degrees >= 179.0 || camera->near_plane <= 0.0 ||
                    camera->far_plane <= camera->near_plane) {
                    result.error = "camera projection values are outside their valid ranges";
                    return result;
                }
            }
        }
        std::optional<MeshRenderer> mesh_renderer;
        if (result.source_version >= 3U) {
            const auto* renderer_value = field(*entity_object, "mesh_renderer");
            if (renderer_value == nullptr) {
                result.error = "version 3 entity mesh_renderer field is missing";
                return result;
            }
            if (!renderer_value->is_null()) {
                const auto* renderer_object = renderer_value->object();
                const auto* mesh = renderer_object == nullptr ? nullptr : field(*renderer_object, "mesh");
                const auto* material = renderer_object == nullptr ? nullptr : field(*renderer_object, "material");
                if (mesh == nullptr || mesh->string() == nullptr || material == nullptr ||
                    material->string() == nullptr || mesh->string()->empty() || material->string()->empty() ||
                    mesh->string()->size() > 128U || material->string()->size() > 128U) {
                    result.error = "mesh renderer requires mesh and material asset identifiers";
                    return result;
                }
                mesh_renderer = MeshRenderer{*mesh->string(), *material->string()};
            }
        }
        slot.generation = parsed_entity->generation;
        slot.alive = true;
        slot.record = EntityRecord{*name->string(), transform, parent, camera, mesh_renderer};
        ++result.entity_count;
    }

    if (result.source_version >= 1U) {
        const auto* allocator_value = scene_object == nullptr ? nullptr : field(*scene_object, "allocator");
        const auto* allocator = allocator_value == nullptr ? nullptr : allocator_value->object();
        const auto* generations_value = allocator == nullptr ? nullptr : field(*allocator, "slot_generations");
        const auto* free_value = allocator == nullptr ? nullptr : field(*allocator, "free_indices");
        if (generations_value == nullptr || generations_value->array() == nullptr ||
            free_value == nullptr || free_value->array() == nullptr ||
            generations_value->array()->size() > static_cast<std::size_t>(max_entity_index) + 1U ||
            generations_value->array()->size() < state.slots.size()) {
            result.error = "scene allocator metadata is missing or invalid";
            return result;
        }
        state.slots.resize(generations_value->array()->size());
        for (std::size_t index = 0; index < state.slots.size(); ++index) {
            std::uint32_t generation = 0;
            if (!read_integer(&(*generations_value->array())[index], generation) || generation == 0U ||
                (state.slots[index].alive && state.slots[index].generation != generation)) {
                result.error = "scene allocator contains an invalid slot generation";
                return result;
            }
            state.slots[index].generation = generation;
        }
        std::vector<bool> free_seen(state.slots.size(), false);
        for (const auto& free_index_value : *free_value->array()) {
            std::uint32_t free_index = 0;
            if (!read_integer(&free_index_value, free_index) || free_index >= state.slots.size() ||
                state.slots[free_index].alive || free_seen[free_index]) {
                result.error = "scene allocator contains an invalid free index";
                return result;
            }
            free_seen[free_index] = true;
            state.free_indices.push_back(free_index);
        }
        for (std::size_t index = 0; index < state.slots.size(); ++index) {
            if (!state.slots[index].alive && !free_seen[index]) {
                result.error = "scene allocator omits a free slot";
                return result;
            }
        }
    } else {
        for (std::uint32_t index = 0; index < state.slots.size(); ++index) {
            if (!state.slots[index].alive) state.free_indices.push_back(index);
        }
    }
    if (!hierarchy_is_valid(state, result.error)) return result;
    std::optional<Entity> active_camera;
    for (std::uint32_t index = 0; index < state.slots.size(); ++index) {
        const auto& slot = state.slots[index];
        if (!slot.alive || !slot.record.camera.has_value() || !slot.record.camera->active) continue;
        if (active_camera.has_value()) {
            result.error = "scene contains more than one active camera";
            return result;
        }
        active_camera = Entity{index, slot.generation};
    }
    result.state = std::move(state);
    return result;
}

bool save_scene_file_atomic(const Scene& scene, const std::filesystem::path& path, std::string& error) {
    error.clear();
    if (path.empty() || path.filename().empty()) {
        error = "scene path must name a file";
        return false;
    }
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = "could not create scene directory: " + filesystem_error.message();
            return false;
        }
    }
    const auto temporary = temporary_path_for(path);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        const auto document = scene.serialize_json();
        output.write(document.data(), static_cast<std::streamsize>(document.size()));
        output.put('\n');
        output.flush();
        if (!output) {
            error = "could not write the complete temporary scene file";
            output.close();
            std::filesystem::remove(temporary, filesystem_error);
            return false;
        }
    }
    if (!replace_atomically(temporary, path, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    return true;
}

} // namespace relay
