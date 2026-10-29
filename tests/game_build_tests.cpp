// Builds the demo project into a standalone game and runs it. The game is started from another
// folder, then from a moved copy, to show it needs nothing but its own folder; it renders
// offscreen (SDL_VIDEODRIVER=offscreen), so no window reaches the desktop.

#include "relay/control/control_protocol.hpp"
#include "relay/core/elf_requirements.hpp"
#include "relay/core/engine.hpp"
#include "relay/audio/audio_clip.hpp"
#include "relay/core/pak.hpp"
#include "relay/core/process.hpp"
#include "relay/observe/capture.hpp"
#include "relay/render/image_decode.hpp"
#include "relay/script/script_system.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string read_text(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

// Copies a project without its outputs.
void copy_project(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    for (fs::recursive_directory_iterator it(from), end; it != end; ++it) {
        const auto relative = it->path().lexically_relative(from);
        const auto first = relative.begin()->string();
        if (first == "builds" || first == "exports" || first == ".relay-cache" || first == ".relay-trash") {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory()) fs::create_directories(to / relative);
        else fs::copy_file(it->path(), to / relative, fs::copy_options::overwrite_existing);
    }
}

// Runs the game; true when it started (or could not for want of a GPU, which skips).
struct GameRun {
    int exit_code{-1};
    std::string output;
    bool no_gpu{};
};

GameRun run_game(const fs::path& executable, const fs::path& capture) {
    const auto result = relay::run_process({executable.string(), "--frames", "120", "--capture", capture.string(), "--no-audio"},
                                           std::chrono::seconds{180}, 256U * 1024U);
    GameRun run;
    run.exit_code = result.started ? result.exit_code : -1;
    run.output = result.output;
    run.no_gpu = run.exit_code != 0 && (result.output.find("cannot create the game window") != std::string::npos);
    return run;
}

// A frame showing the scene, not just its sky and particles: the bottom third holds the ground
// and the objects standing on it, so it varies a lot, where a sky alone is a smooth gradient.
bool picture_has_content(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream), {}};
    relay::TextureAsset picture;
    std::string error;
    if (bytes.empty() || !relay::decode_image_rgba(bytes, "png", picture, error) || picture.width == 0U) return false;
    double sum = 0.0;
    double squares = 0.0;
    std::size_t count = 0;
    for (std::uint32_t y = picture.height * 2U / 3U; y < picture.height; ++y)
        for (std::uint32_t x = 0; x < picture.width; ++x) {
            const auto* pixel = &picture.rgba[(static_cast<std::size_t>(y) * picture.width + x) * 4U];
            const double luminance = 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2];
            sum += luminance;
            squares += luminance * luminance;
            ++count;
        }
    if (count == 0U) return false;
    const double mean = sum / static_cast<double>(count);
    return std::sqrt(std::max(0.0, squares / static_cast<double>(count) - mean * mean)) > 12.0;
}

} // namespace

int main() {
    const auto temporary = fs::temp_directory_path() /
        ("relay-game-build-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temporary);
    // Trust is per user, outside every project; the test keeps its own.
    const auto trust = (temporary / "trusted").string();
    setenv("RELAY_SCRIPT_TRUST_PATH", trust.c_str(), 1);
    setenv("SDL_VIDEODRIVER", "offscreen", 1);
    setenv("RELAY_AUDIO", "0", 1);

    const fs::path demo = RELAY_DEMO_DIRECTORY;
    const auto project_folder = temporary / "work" / "demo";
    copy_project(demo, project_folder);
    const auto original = fs::current_path();
    fs::current_path(temporary / "work");

    {
        relay::EngineConfig config;
        config.editor_mode = true;
        relay::Engine engine(config);
        relay::ControlProtocol protocol(engine);
        expect(protocol.handle(R"({"id":1,"method":"project.open","filename":"demo/demo.relayproject"})").find(R"("ok":true)") != std::string::npos,
               "the demo project opens");
        std::string error;
        expect(relay::set_project_scripts_trusted(project_folder, true, error), "the project can be trusted: " + error);
        expect(protocol.handle(R"({"id":2,"method":"project.set_build_settings","game_name":"Showcase Game","version":"3.2.1","author":"Tester"})").find(R"("ok":true)") != std::string::npos,
               "the game's details are set");
        expect(protocol.handle(R"({"id":3,"method":"project.set_build_profile","name":"Linux","configuration":"release","output_directory":"builds","window_mode":"windowed","window_width":960,"window_height":540})").find(R"("ok":true)") != std::string::npos,
               "the window size is set");
        const auto built = protocol.handle(R"({"id":4,"method":"project.build"})");
        expect(built.find(R"("ok":true)") != std::string::npos && built.find(R"("executable":true)") != std::string::npos,
               "project.build makes a game: " + built);
    }

    const auto game = project_folder / "builds" / "release";
    const auto executable = game / "Showcase_Game";
    expect(fs::is_regular_file(executable) && (fs::status(executable).permissions() & fs::perms::owner_exec) != fs::perms::none,
           "the game's program is named for the game and can run");
    expect(fs::is_regular_file(game / "build.json") && fs::is_regular_file(game / "game.pak") &&
               fs::is_regular_file(game / "scripts" / "relay-scripts.so") && fs::is_directory(game / "shaders") &&
               fs::is_directory(game / "lib"),
           "the build holds the program, archive, scripts, shaders and libraries");
    expect(!fs::exists(game / "data") && !fs::exists(game / "builds"),
           "the game's files are in the archive, not loose, and earlier builds are not shipped");
    {
        std::string error;
        const auto archive = relay::PakArchive::open(game / "game.pak", error);
        expect(archive != nullptr, "the archive opens: " + error);
        if (archive) {
            std::string problem;
            expect(archive->verify(problem), "every packed file matches its checksum: " + problem);
            std::set<std::string> names;
            for (const auto& entry : archive->entries()) names.insert(entry.name);
            expect(names.contains("project.relayproject") && names.contains("scenes/showcase.relay.json") &&
                       names.contains(".cooked/models.bin") && names.contains(".cooked/shaders/shaders/PlasmaOrb.relay-shader.bin"),
                   "the archive holds the project, its scenes and the cooked models and shaders");
            expect(!names.contains("models/primitives.glb") && !names.contains(".relay-imports.json") &&
                       !names.contains("shaders/PlasmaOrb.relay-shader"),
                   "sources that shipped cooked are not shipped");
            expect(std::none_of(names.begin(), names.end(), [](const std::string& name) { return name.starts_with("scripts/") || name.starts_with("builds/"); }),
                   "script sources and earlier builds are not shipped");
            // Sounds ship as lossless FLAC under their own names, and decode to the very same samples.
            for (const char* sound : {"sounds/hum.wav", "sounds/shot.wav", "music/daylight.wav"}) {
                const auto packed_bytes = archive->find(sound);
                expect(packed_bytes && packed_bytes->size() > 4U && std::memcmp(packed_bytes->data(), "fLaC", 4) == 0 &&
                           packed_bytes->size() < fs::file_size(demo / sound),
                       std::string{"a wav sound ships as smaller FLAC: "} + sound);
                if (!packed_bytes) continue;
                std::ifstream original(demo / sound, std::ios::binary);
                const std::vector<std::uint8_t> wav{std::istreambuf_iterator<char>(original), {}};
                std::string decode_error;
                const auto before = relay::decode_audio(wav, decode_error);
                const auto after = relay::decode_audio(std::vector<std::uint8_t>(packed_bytes->begin(), packed_bytes->end()), decode_error);
                expect(before && after && before->samples == after->samples && before->sample_rate == after->sample_rate &&
                           before->channels == after->channels,
                       std::string{"and decodes to the same samples: "} + sound);
            }
            expect(read_text(game / "build.json").find("\"sounds_compressed\": 6") != std::string::npos,
                   "build.json counts the compressed sounds");
        }
    }
    if (fs::is_directory(game / "shaders") && fs::is_directory(game / "lib")) {
        std::size_t shaders = 0;
        for (const auto& entry : fs::directory_iterator(game / "shaders")) shaders += entry.path().extension() == ".spv" ? 1U : 0U;
        expect(shaders >= 10U, "the compiled shaders ship with the game");
        bool has_sdl = false;
        for (const auto& entry : fs::directory_iterator(game / "lib")) has_sdl = has_sdl || entry.path().filename().string().rfind("libSDL3", 0) == 0;
        expect(has_sdl, "libraries the player needs ship beside it");
        expect(!fs::exists(game / "lib" / "libc.so.6") && !fs::exists(game / "lib" / "libvulkan.so.1"),
               "system libraries and the GPU loader stay out");
        bool importer_shipped = false;
        for (const auto& entry : fs::directory_iterator(game / "lib")) {
            const auto name = entry.path().filename().string();
            importer_shipped = importer_shipped || name.rfind("libassimp", 0) == 0 || name.rfind("libglslang", 0) == 0 ||
                               name.rfind("libSPIRV", 0) == 0;
        }
        expect(!importer_shipped, "the model importer and the shader compiler are not shipped: games read cooked files");
    }
    const auto manifest = read_text(game / "build.json");
    expect(manifest.find("\"executable\": \"Showcase_Game\"") != std::string::npos &&
               manifest.find("\"scripts\": \"scripts/relay-scripts.so\"") != std::string::npos &&
               manifest.find("\"version\": \"3.2.1\"") != std::string::npos,
           "build.json names the program and scripts: " + manifest);

    // Run it from somewhere else, then from a moved copy.
    fs::current_path(temporary);
    const auto first = run_game(executable, temporary / "first.png");
    if (first.no_gpu) {
        std::cout << "SKIP: no Vulkan device for the game window: " << first.output << '\n';
    } else {
        expect(first.exit_code == 0, "the game runs and exits cleanly: " + first.output);
        expect(picture_has_content(temporary / "first.png"), "the game draws its scene");
        const auto moved = temporary / "moved" / "Showcase Game folder";
        fs::create_directories(moved.parent_path());
        if (std::getenv("RELAY_KEEP_GAME_BUILD")) fs::copy(game, moved, fs::copy_options::recursive);
        else fs::rename(game, moved);
        const auto second = run_game(moved / "Showcase_Game", temporary / "second.png");
        expect(second.exit_code == 0, "the game runs after its folder moves: " + second.output);
        expect(picture_has_content(temporary / "second.png"), "the moved game draws its scene");
    }

    // A textured model, built once with compressed textures and once without: the GPU shows the same
    // picture, and the compressed game's archive is smaller.
    {
        fs::current_path(temporary / "work");
        fs::create_directories("textured/models");
        std::string error;
        relay::OwnedFrame texture;
        texture.width = texture.height = 128;
        texture.rgba.resize(128U * 128U * 4U);
        for (std::uint32_t y = 0; y < 128U; ++y)
            for (std::uint32_t x = 0; x < 128U; ++x) {
                auto* p = &texture.rgba[(static_cast<std::size_t>(y) * 128U + x) * 4U];
                const bool tile = ((x / 16U) + (y / 16U)) % 2U == 0U;
                p[0] = static_cast<std::uint8_t>(tile ? 230U : 40U + x);
                p[1] = static_cast<std::uint8_t>(60U + y);
                p[2] = static_cast<std::uint8_t>(tile ? 40U : 200U);
                p[3] = 255U;
            }
        expect(relay::write_frame_image(texture.view(), "textured/models/tex.png", error), "the test texture is written: " + error);
        {
            auto gltf = read_text(fs::path{RELAY_FIXTURE_MODELS} / "relay-test-triangle.gltf");
            const auto swap = [&](const std::string& from, const std::string& to) {
                const auto at = gltf.find(from);
                if (at != std::string::npos) gltf.replace(at, from.size(), to);
            };
            swap("\"materials\"", "\"images\":[{\"uri\":\"tex.png\"}],\"textures\":[{\"source\":0}],\"materials\"");
            swap("\"baseColorFactor\"", "\"baseColorTexture\":{\"index\":0},\"baseColorFactor\"");
            std::ofstream("textured/models/quad.gltf") << gltf;
        }
        relay::EngineConfig config;
        config.editor_mode = true;
        relay::Engine engine(config);
        relay::ControlProtocol protocol(engine);
        const auto ok = [](const std::string& response) { return response.find("\"ok\":true") != std::string::npos; };
        expect(ok(protocol.handle(R"({"id":1,"method":"project.create","filename":"textured/project.relayproject","name":"Textured"})")),
               "the textured project is created");
        const auto imported = protocol.handle(R"({"id":2,"method":"assets.import_model","filename":"models/quad.gltf","instantiate":true})");
        expect(ok(imported), "the textured model is imported");
        const auto camera = protocol.handle(R"({"id":3,"method":"scene.create","name":"View","type":"Camera"})");
        const auto at = camera.find("\"entity\":\"");
        expect(at != std::string::npos, "a camera is added");
        if (at != std::string::npos) {
            const auto handle = camera.substr(at + 10U, camera.find('"', at + 10U) - at - 10U);
            expect(ok(protocol.handle(R"({"id":4,"method":"scene.set_transform","entity":")" + handle + R"(","pz":1.4})")), "the camera is moved back");
        }
        expect(ok(protocol.handle(R"({"id":5,"method":"scene.save","filename":"main.relay.json"})")) &&
                   ok(protocol.handle(R"({"id":6,"method":"project.add_scene","scene_file":"main.relay.json"})")) &&
                   ok(protocol.handle(R"({"id":7,"method":"project.set_startup","scene_file":"main.relay.json"})")) &&
                   ok(protocol.handle(R"({"id":8,"method":"project.set_build_profile","name":"Linux","configuration":"release","output_directory":"builds","window_width":640,"window_height":360})")),
               "the scene is saved and made the startup scene");

        const auto build = [&](const bool compressed, const fs::path& capture) {
            struct Result { bool built{}; std::uint64_t archive{}; GameRun run; std::string manifest; };
            Result result;
            const auto response = protocol.handle(std::string{R"({"id":9,"method":"project.set_build_profile","name":"Linux","compress_textures":)"} +
                                                  (compressed ? "true" : "false") + "}");
            result.built = ok(response) && ok(protocol.handle(R"({"id":10,"method":"project.build"})"));
            const auto folder = fs::path{"textured/builds/release"};
            std::error_code ec;
            result.archive = fs::file_size(folder / "game.pak", ec);
            result.manifest = read_text(folder / "build.json");
            result.run = run_game(folder / "Textured", fs::absolute(capture));
            return result;
        };
        setenv("RELAY_GLOBAL_ILLUMINATION", "0", 1);
        setenv("RELAY_REFLECTIONS", "0", 1);
        const auto packed = build(true, temporary / "textured-bc.png");
        const auto plain = build(false, temporary / "textured-raw.png");
        unsetenv("RELAY_GLOBAL_ILLUMINATION");
        unsetenv("RELAY_REFLECTIONS");
        expect(packed.built && plain.built, "the textured game builds both ways");
        // The glibc the game needs is measured from its files, and a profile can set a floor under it.
        {
            relay::ElfRequirements player_needs;
            std::string elf_error;
            const auto player = fs::path{RELAY_PLAYER_BINARY_PATH};
            expect(relay::read_elf_requirements(player, player_needs, elf_error), "the player's requirements are readable: " + elf_error);
            const std::string key = "\"glibc\": \"";
            const auto marker = packed.manifest.find(key);
            const auto recorded = marker == std::string::npos ? std::string{}
                                  : packed.manifest.substr(marker + key.size(), packed.manifest.find('"', marker + key.size()) - marker - key.size());
            expect(!recorded.empty() && relay::compare_dotted_versions(recorded, player_needs.glibc) >= 0,
                   "build.json records at least the glibc the player itself needs: " + recorded + " against " + player_needs.glibc);
            expect(ok(protocol.handle(R"({"id":12,"method":"project.set_build_profile","name":"Linux","minimum_glibc":"2.17"})")), "a minimum glibc is set");
            const auto refused = protocol.handle(R"({"id":13,"method":"project.build"})");
            expect(refused.find("\"ok\":false") != std::string::npos && refused.find("needs glibc") != std::string::npos,
                   "a build that needs newer glibc than the profile's minimum fails: " + refused);
            expect(ok(protocol.handle(R"({"id":14,"method":"project.set_build_profile","name":"Linux","minimum_glibc":""})")), "and the minimum is cleared");
        }
        expect(packed.manifest.find("\"textures_compressed\": 1") != std::string::npos &&
                   plain.manifest.find("\"textures_compressed\": 0") != std::string::npos,
               "build.json says whether the textures were compressed");
        expect(packed.archive + 40U * 1024U < plain.archive, "the compressed archive is smaller: " + std::to_string(packed.archive) + " against " + std::to_string(plain.archive));
        if (packed.run.no_gpu) {
            std::cout << "SKIP: no Vulkan device for the textured game\n";
        } else {
            expect(packed.run.exit_code == 0 && plain.run.exit_code == 0, "both textured games run: " + packed.run.output + plain.run.output);
            expect(packed.run.output.find("\"compressed_textures\":1") != std::string::npos &&
                       plain.run.output.find("\"compressed_textures\":0") != std::string::npos,
                   "the GPU took the compressed texture as blocks: " + packed.run.output);
            const auto pixels = [&](const fs::path& path) {
                std::ifstream stream(path, std::ios::binary);
                const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream), {}};
                relay::TextureAsset picture;
                std::string decode_error;
                (void)relay::decode_image_rgba(bytes, "png", picture, decode_error);
                return picture;
            };
            const auto a = pixels(temporary / "textured-bc.png"), b = pixels(temporary / "textured-raw.png");
            double difference = 0.0;
            std::size_t lit = 0;
            for (std::size_t i = 0; i + 3U < a.rgba.size() && a.rgba.size() == b.rgba.size(); i += 4U) {
                difference += std::abs(static_cast<int>(a.rgba[i]) - b.rgba[i]) + std::abs(static_cast<int>(a.rgba[i + 1U]) - b.rgba[i + 1U]) +
                              std::abs(static_cast<int>(a.rgba[i + 2U]) - b.rgba[i + 2U]);
                lit += (a.rgba[i] > 90U || a.rgba[i + 1U] > 90U) ? 1U : 0U;
            }
            const auto count = std::max<std::size_t>(a.rgba.size() / 4U, 1U);
            expect(a.rgba.size() == b.rgba.size() && !a.rgba.empty() && lit > count / 40U,
                   "the textured model is on screen");
            std::cout << "textured game: archive " << packed.archive << " bytes compressed, " << plain.archive
                      << " plain; mean picture difference " << difference / static_cast<double>(count * 3U) << " of 255\n";
            expect(difference / static_cast<double>(count * 3U) < 3.0,
                   "compressed and plain textures draw the same picture: mean difference " +
                       std::to_string(difference / static_cast<double>(count * 3U)));
        }
    }

    fs::current_path(original);
    std::error_code ignored;
    // RELAY_KEEP_GAME_BUILD keeps the folder, for looking at what was built.
    if (std::getenv("RELAY_KEEP_GAME_BUILD")) std::cout << "kept " << temporary << '\n';
    else fs::remove_all(temporary, ignored);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "Game build tests passed\n";
    return 0;
}
