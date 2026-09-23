#pragma once

#include "relay/scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace relay {

inline constexpr std::uint32_t scene_file_version = 15;

struct SceneFileLoadResult {
    std::optional<SceneState> state;
    std::uint32_t source_version{0};
    bool migrated{false};
    std::size_t entity_count{0};
    std::string error;

    [[nodiscard]] explicit operator bool() const { return state.has_value(); }
};

[[nodiscard]] SceneFileLoadResult load_scene_file(const std::filesystem::path& path);
[[nodiscard]] bool save_scene_file_atomic(const Scene& scene, const std::filesystem::path& path,
                                          std::string& error);

} // namespace relay
