#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace relay {

struct BlenderConversionSettings {
    // Empty selects RELAY_BLENDER_EXECUTABLE, then falls back to `blender` on PATH.
    std::filesystem::path executable;
    std::uint32_t timeout_seconds{120U};
    bool export_animations{true};
    bool export_cameras{true};
    bool export_lights{true};
    // Explicit trusted-input escape hatch for hosts without the Linux Bubblewrap boundary.
    // Never exposed as an agent-controlled protocol parameter.
    bool allow_unsandboxed{false};
};

struct BlenderConversionResult {
    bool converted{};
    bool cache_hit{};
    bool sandboxed{};
    // Canonicalized path relative to the project assets root.
    std::filesystem::path converted_model;
    std::string blender_version;
    std::string diagnostics;
    std::vector<std::filesystem::path> dependencies;
};

// Converts one top-level .blend file to a content-addressed GLB below
// assets/.relay-cache/blender. The subprocess receives fixed Blender flags, never a shell command,
// and cannot execute Python embedded in the source file.
[[nodiscard]] BlenderConversionResult convert_blend_to_glb(
    const std::filesystem::path& assets_root, const std::filesystem::path& source,
    const BlenderConversionSettings& settings, std::string& error);

} // namespace relay
