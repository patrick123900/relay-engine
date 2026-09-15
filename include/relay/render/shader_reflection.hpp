#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace relay {

struct ShaderLocation {
    std::uint32_t location{};
    std::string name;
};

struct ShaderBinding {
    std::uint32_t set{};
    std::uint32_t binding{};
    std::string name;
};

struct ShaderInterface {
    bool valid{false};
    std::string error;
    std::string stage;
    std::uint32_t push_constant_bytes{};
    std::vector<ShaderLocation> inputs;
    std::vector<ShaderLocation> outputs;
    std::vector<ShaderBinding> bindings;

    [[nodiscard]] std::string json() const;
};

[[nodiscard]] ShaderInterface reflect_spirv(std::span<const std::uint32_t> words);

} // namespace relay
