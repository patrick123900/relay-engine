#include "relay/render/shader_reflection.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace relay {
namespace {

constexpr std::uint32_t spirv_magic = 0x07230203U;
constexpr std::uint16_t op_name = 5U;
constexpr std::uint16_t op_entry_point = 15U;
constexpr std::uint16_t op_type_int = 21U;
constexpr std::uint16_t op_type_float = 22U;
constexpr std::uint16_t op_type_vector = 23U;
constexpr std::uint16_t op_type_matrix = 24U;
constexpr std::uint16_t op_type_array = 28U;
constexpr std::uint16_t op_type_struct = 30U;
constexpr std::uint16_t op_type_pointer = 32U;
constexpr std::uint16_t op_constant = 43U;
constexpr std::uint16_t op_variable = 59U;
constexpr std::uint16_t op_decorate = 71U;
constexpr std::uint16_t op_member_decorate = 72U;
constexpr std::uint32_t decoration_array_stride = 6U;
constexpr std::uint32_t decoration_location = 30U;
constexpr std::uint32_t decoration_binding = 33U;
constexpr std::uint32_t decoration_descriptor_set = 34U;
constexpr std::uint32_t decoration_offset = 35U;
constexpr std::uint32_t storage_uniform_constant = 0U;
constexpr std::uint32_t storage_input = 1U;
constexpr std::uint32_t storage_uniform = 2U;
constexpr std::uint32_t storage_output = 3U;
constexpr std::uint32_t storage_push_constant = 9U;
constexpr std::uint32_t storage_storage_buffer = 12U;

struct Type {
    enum class Kind { unknown, scalar, vector, matrix, array, structure, pointer } kind{Kind::unknown};
    std::uint32_t width{};
    std::uint32_t element{};
    std::uint32_t count{};
    std::uint32_t storage{};
    std::vector<std::uint32_t> members;
};

struct Decorations {
    std::optional<std::uint32_t> location;
    std::optional<std::uint32_t> binding;
    std::optional<std::uint32_t> set;
    std::optional<std::uint32_t> array_stride;
};

std::string decode_string(const std::span<const std::uint32_t> words) {
    std::string output;
    for (const auto word : words) {
        for (unsigned byte = 0; byte < 4U; ++byte) {
            const char character = static_cast<char>((word >> (byte * 8U)) & 0xFFU);
            if (character == '\0') return output;
            output += character;
        }
    }
    return output;
}

std::string escape_json(const std::string_view text) {
    std::string output;
    for (const char character : text) {
        if (character == '"' || character == '\\') output += '\\';
        output += character;
    }
    return output;
}

std::uint32_t type_size(
    const std::uint32_t id, const std::unordered_map<std::uint32_t, Type>& types,
    const std::unordered_map<std::uint32_t, std::uint32_t>& constants,
    const std::unordered_map<std::uint32_t, Decorations>& decorations,
    const std::unordered_map<std::uint64_t, std::uint32_t>& member_offsets) {
    const auto found = types.find(id);
    if (found == types.end()) return 0U;
    const auto& type = found->second;
    switch (type.kind) {
    case Type::Kind::scalar: return type.width / 8U;
    case Type::Kind::vector:
    case Type::Kind::matrix: return type_size(type.element, types, constants, decorations, member_offsets) * type.count;
    case Type::Kind::array: {
        const auto count = constants.find(type.count);
        if (count == constants.end()) return 0U;
        const auto decorated = decorations.find(id);
        const auto stride = decorated != decorations.end() && decorated->second.array_stride.has_value()
                                ? *decorated->second.array_stride
                                : type_size(type.element, types, constants, decorations, member_offsets);
        return stride * count->second;
    }
    case Type::Kind::structure: {
        std::uint32_t size = 0U;
        for (std::size_t index = 0; index < type.members.size(); ++index) {
            const std::uint64_t key = (static_cast<std::uint64_t>(id) << 32U) | index;
            const auto offset = member_offsets.find(key);
            const auto member_size = type_size(type.members[index], types, constants, decorations,
                                               member_offsets);
            size = std::max(size, (offset == member_offsets.end() ? size : offset->second) + member_size);
        }
        return size;
    }
    case Type::Kind::pointer: return type_size(type.element, types, constants, decorations, member_offsets);
    case Type::Kind::unknown: return 0U;
    }
    return 0U;
}

} // namespace

ShaderInterface reflect_spirv(const std::span<const std::uint32_t> words) {
    ShaderInterface output;
    if (words.size() < 5U || words.front() != spirv_magic) {
        output.error = "shader is not a valid SPIR-V module";
        return output;
    }
    std::unordered_map<std::uint32_t, Type> types;
    std::unordered_map<std::uint32_t, std::uint32_t> constants;
    std::unordered_map<std::uint32_t, Decorations> decorations;
    std::unordered_map<std::uint64_t, std::uint32_t> member_offsets;
    std::unordered_map<std::uint32_t, std::string> names;
    struct Variable { std::uint32_t type{}; std::uint32_t storage{}; };
    std::unordered_map<std::uint32_t, Variable> variables;

    for (std::size_t cursor = 5U; cursor < words.size();) {
        const auto instruction = words[cursor];
        const auto count = static_cast<std::uint16_t>(instruction >> 16U);
        const auto opcode = static_cast<std::uint16_t>(instruction & 0xFFFFU);
        if (count == 0U || cursor + count > words.size()) {
            output.error = "SPIR-V instruction extends past the module boundary";
            return output;
        }
        const auto operand = [&](const std::size_t index) { return words[cursor + index]; };
        if (opcode == op_entry_point && count >= 3U) {
            if (operand(1U) == 0U) output.stage = "vertex";
            else if (operand(1U) == 4U) output.stage = "fragment";
            else output.stage = "other";
        } else if (opcode == op_name && count >= 3U) {
            names[operand(1U)] = decode_string(words.subspan(cursor + 2U, count - 2U));
        } else if ((opcode == op_type_int || opcode == op_type_float) && count >= 3U) {
            Type type;
            type.kind = Type::Kind::scalar;
            type.width = operand(2U);
            types[operand(1U)] = std::move(type);
        } else if ((opcode == op_type_vector || opcode == op_type_matrix) && count >= 4U) {
            Type type;
            type.kind = opcode == op_type_vector ? Type::Kind::vector : Type::Kind::matrix;
            type.element = operand(2U);
            type.count = operand(3U);
            types[operand(1U)] = std::move(type);
        } else if (opcode == op_type_array && count >= 4U) {
            Type type;
            type.kind = Type::Kind::array;
            type.element = operand(2U);
            type.count = operand(3U);
            types[operand(1U)] = std::move(type);
        } else if (opcode == op_type_struct && count >= 2U) {
            Type type;
            type.kind = Type::Kind::structure;
            for (std::size_t index = 2U; index < count; ++index) type.members.push_back(operand(index));
            types[operand(1U)] = std::move(type);
        } else if (opcode == op_type_pointer && count >= 4U) {
            Type type;
            type.kind = Type::Kind::pointer;
            type.storage = operand(2U);
            type.element = operand(3U);
            types[operand(1U)] = std::move(type);
        } else if (opcode == op_constant && count >= 4U) {
            constants[operand(2U)] = operand(3U);
        } else if (opcode == op_variable && count >= 4U) {
            variables[operand(2U)] = {operand(1U), operand(3U)};
        } else if (opcode == op_decorate && count >= 3U) {
            auto& decorated = decorations[operand(1U)];
            if (operand(2U) == decoration_location && count >= 4U) decorated.location = operand(3U);
            else if (operand(2U) == decoration_binding && count >= 4U) decorated.binding = operand(3U);
            else if (operand(2U) == decoration_descriptor_set && count >= 4U) decorated.set = operand(3U);
            else if (operand(2U) == decoration_array_stride && count >= 4U) decorated.array_stride = operand(3U);
        } else if (opcode == op_member_decorate && count >= 5U && operand(3U) == decoration_offset) {
            const std::uint64_t key = (static_cast<std::uint64_t>(operand(1U)) << 32U) | operand(2U);
            member_offsets[key] = operand(4U);
        }
        cursor += count;
    }

    for (const auto& [id, variable] : variables) {
        const auto decorated = decorations.find(id);
        const auto name = names.contains(id) ? names.at(id) : std::string{};
        if (variable.storage == storage_push_constant) {
            output.push_constant_bytes = std::max(
                output.push_constant_bytes,
                type_size(variable.type, types, constants, decorations, member_offsets));
        } else if (decorated != decorations.end() && decorated->second.location.has_value()) {
            ShaderLocation location{*decorated->second.location, name};
            if (variable.storage == storage_input) output.inputs.push_back(std::move(location));
            else if (variable.storage == storage_output) output.outputs.push_back(std::move(location));
        }
        if ((variable.storage == storage_uniform_constant || variable.storage == storage_uniform ||
             variable.storage == storage_storage_buffer) && decorated != decorations.end() &&
            decorated->second.binding.has_value()) {
            output.bindings.push_back({decorated->second.set.value_or(0U),
                                       *decorated->second.binding, name});
        }
    }
    const auto by_location = [](const auto& left, const auto& right) { return left.location < right.location; };
    std::sort(output.inputs.begin(), output.inputs.end(), by_location);
    std::sort(output.outputs.begin(), output.outputs.end(), by_location);
    std::sort(output.bindings.begin(), output.bindings.end(), [](const auto& left, const auto& right) {
        return left.set < right.set || (left.set == right.set && left.binding < right.binding);
    });
    output.valid = !output.stage.empty();
    if (!output.valid) output.error = "SPIR-V module has no supported entry point";
    return output;
}

std::string ShaderInterface::json() const {
    std::ostringstream output;
    output << "{\"valid\":" << (valid ? "true" : "false") << ",\"error\":\""
           << escape_json(error) << "\",\"stage\":\"" << stage
           << "\",\"push_constant_bytes\":" << push_constant_bytes << ",\"inputs\":[";
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"location\":" << inputs[index].location << ",\"name\":\""
               << escape_json(inputs[index].name) << "\"}";
    }
    output << "],\"outputs\":[";
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"location\":" << outputs[index].location << ",\"name\":\""
               << escape_json(outputs[index].name) << "\"}";
    }
    output << "],\"bindings\":[";
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"set\":" << bindings[index].set << ",\"binding\":"
               << bindings[index].binding << ",\"name\":\""
               << escape_json(bindings[index].name) << "\"}";
    }
    output << "]}";
    return output.str();
}

} // namespace relay
