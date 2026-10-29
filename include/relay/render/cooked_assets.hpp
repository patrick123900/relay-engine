#pragma once

#include "relay/render/assets.hpp"
#include "relay/render/shader_language.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// What a build makes of a project's models and shaders, so the game reads them ready to use
// instead of importing and compiling them on every start. Both live in the game archive.
inline constexpr std::string_view cooked_models_name = ".cooked/models.bin";
inline constexpr std::string_view cooked_shader_directory = ".cooked/shaders/";

// Everything the importer registered, in order: batches of meshes, materials and textures, and the
// models that describe their nodes and animations. Registering the same steps in the same order
// gives a registry identical to the one the importer built, with the same identities.
struct CookedModels {
    struct Step {
        std::optional<ImportBatch> batch;
        std::optional<ModelAsset> model;
    };
    std::vector<Step> steps;
    std::uint32_t models{}; // Imported model files these steps came from.
};

[[nodiscard]] std::string serialize_cooked_models(const CookedModels& cooked);
[[nodiscard]] bool parse_cooked_models(std::span<const std::uint8_t> bytes, CookedModels& cooked,
                                       std::string& error);
void apply_cooked_models(const CookedModels& cooked, AssetRegistry& registry);

// Imports the models of the project at `root` that `sources` names (project-relative model files)
// with the importer, and writes down what it registers. Fails when a model no longer matches its
// import record, since a game cannot rebind scenes the way the editor does.
// What cooking the textures did.
struct CookStats {
    std::uint32_t textures{};
    std::uint32_t textures_compressed{};
    std::uint64_t texture_bytes_before{};
    std::uint64_t texture_bytes_after{};
};
// With `compress_textures`, colour and data textures of the models become BC1 or BC3 with mip
// chains (see texture_compression.hpp). Normal maps stay as they are: the shaders read all three
// channels, which these formats keep too coarsely, and tiny textures are not worth the trouble.
[[nodiscard]] bool cook_models(const std::filesystem::path& root, const std::set<std::string>& sources,
                               std::string& bytes, std::string& error, bool compress_textures = false,
                               CookStats* stats = nullptr);
// Compresses the textures of one imported batch in place (what cook_models does for each).
void compress_batch_textures(ImportBatch& batch, CookStats* stats = nullptr);

// A compiled project shader, without the source text and editing data.
[[nodiscard]] std::string serialize_cooked_shader(const CompiledShader& shader);
[[nodiscard]] std::shared_ptr<const CompiledShader> parse_cooked_shader(std::span<const std::uint8_t> bytes,
                                                                        std::string& error);
// Compiles the project's shader `path` and serializes it; fails with the shader's own errors.
[[nodiscard]] bool cook_shader(const std::filesystem::path& root, std::string_view path,
                               std::string& bytes, std::string& error);

} // namespace relay
