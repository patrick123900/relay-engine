// The player runs a game that Run > Build project made. It sits in the build folder beside
// build.json, data/ (the game's project), scripts/ (its compiled behaviours), shaders/ and lib/,
// and plays the project the way Run Game does in the editor, without the editor.

#include "../common/window_hooks.hpp"
#include "relay/control/control_protocol.hpp"
#include "relay/core/pak.hpp"
#include "relay/core/engine.hpp"
#include "relay/core/json.hpp"
#include "relay/platform/vulkan_window.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;

struct Manifest {
    std::string name{"Relay game"};
    std::string version;
    std::string configuration{"release"};
    std::string window_mode{"windowed"};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    bool icon{};
    std::string archive{"game.pak"};
    std::string root{"data"};
    std::string project{"project.relayproject"};
    std::string scripts;
};

std::string string_or(const relay::JsonValue::Object& object, const std::string_view name,
                      const std::string& fallback = {}) {
    const auto* entry = relay::field(object, name);
    return entry && entry->string() ? *entry->string() : fallback;
}

double number_or(const relay::JsonValue::Object& object, const std::string_view name, const double fallback) {
    const auto* entry = relay::field(object, name);
    return entry && entry->number() ? *entry->number() : fallback;
}

fs::path executable_directory() {
    std::error_code ec;
#ifdef _WIN32
    return fs::current_path(ec);
#else
    const auto self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path(ec) : self.parent_path();
#endif
}

std::optional<Manifest> read_manifest(const fs::path& file, std::string& error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "there is no build.json beside the game; run the player from a build folder";
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>(stream), {}};
    relay::JsonParser parser(text);
    const auto value = parser.parse();
    const auto* root = value ? value->object() : nullptr;
    if (!root || string_or(*root, "format") != "relay.build") {
        error = "build.json is not a Relay build manifest";
        return std::nullopt;
    }
    Manifest manifest;
    const auto section = [&](const char* name) -> const relay::JsonValue::Object* {
        const auto* entry = relay::field(*root, name);
        return entry ? entry->object() : nullptr;
    };
    if (const auto* game = section("game")) {
        manifest.name = string_or(*game, "name", manifest.name);
        manifest.version = string_or(*game, "version");
        const auto* icon = relay::field(*game, "icon");
        manifest.icon = icon && icon->string();
    }
    if (const auto* window = section("window")) {
        manifest.window_mode = string_or(*window, "mode", manifest.window_mode);
        manifest.width = static_cast<std::uint32_t>(number_or(*window, "width", manifest.width));
        manifest.height = static_cast<std::uint32_t>(number_or(*window, "height", manifest.height));
    }
    if (const auto* target = section("target"))
        manifest.configuration = string_or(*target, "configuration", manifest.configuration);
    if (const auto* content = section("content")) {
        manifest.archive = string_or(*content, "archive", manifest.archive);
        manifest.root = string_or(*content, "root", manifest.root);
        manifest.project = string_or(*content, "project", manifest.project);
    }
    if (const auto* scripts = relay::field(*root, "scripts"); scripts && scripts->string())
        manifest.scripts = *scripts->string();
    return manifest;
}

void print_logs(const relay::Engine& engine, std::uint64_t& cursor, const bool everything) {
    for (const auto& entry : engine.logs().read_after(cursor)) {
        cursor = std::max(cursor, entry.sequence);
        if (everything || entry.level != relay::LogLevel::info)
            std::cerr << '[' << relay::to_string(entry.level) << "] " << entry.message << '\n';
    }
}

void usage() {
    std::cout << "usage: <game> [--frames N] [--capture file.png] [--no-audio] [--verify] [--version]\n"
                 "Plays the game in this folder. --frames N quits after N frames, and --capture\n"
                 "saves the last one; both are for automated checks.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t frame_limit = 0;
    std::string capture_path;
    bool audio = true;
    bool verify = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            usage();
            return 0;
        }
        if (argument == "--no-audio") audio = false;
        else if (argument == "--verify") verify = true;
        else if (argument == "--frames" && i + 1 < argc) frame_limit = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (argument == "--capture" && i + 1 < argc) capture_path = argv[++i];
        else if (argument == "--version") {
            std::string error;
            if (const auto manifest = read_manifest(executable_directory() / "build.json", error))
                std::cout << manifest->name << ' ' << manifest->version << '\n';
            else
                std::cerr << error << '\n';
            return 0;
        } else {
            std::cerr << "unknown option " << argument << '\n';
            usage();
            return 2;
        }
    }
    if (const char* quiet = std::getenv("RELAY_AUDIO"); quiet && std::string_view(quiet) == "0") audio = false;

    // Everything the game reads is relative to its folder, whatever the working directory was.
    const auto folder = executable_directory();
    std::string error;
    const auto manifest = read_manifest(folder / "build.json", error);
    if (!manifest) {
        std::cerr << error << '\n';
        return 1;
    }
    std::error_code ec;
    fs::current_path(folder, ec);
    if (ec) {
        std::cerr << "cannot enter the game folder: " << ec.message() << '\n';
        return 1;
    }
    const bool debug = manifest->configuration == "debug";

    // The game's files are in its archive, which appears as the folder `root` beside the program.
    const auto archive = relay::PakArchive::open(folder / manifest->archive, error);
    if (!archive) {
        std::cerr << "cannot open the game's files: " << error << '\n';
        return 1;
    }
    if (verify) {
        std::string problem;
        if (!archive->verify(problem)) {
            std::cerr << "the game's files are damaged: " << problem << '\n';
            return 1;
        }
        std::cerr << "The game's " << archive->entries().size() << " files match their checksums\n";
    }
    relay::vfs::mount(archive, folder / manifest->root);

    relay::EngineConfig config;
    config.editor_mode = true; // Run Game starts from the editor mode; the player starts it at once.
    config.audio_output = audio;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    const auto opened = protocol.handle(R"({"id":1,"method":"project.open","filename":")" +
                                        relay::json_escape(manifest->root + "/" + manifest->project) + "\"}");
    if (opened.find(R"("ok":true)") == std::string::npos) {
        std::cerr << "cannot open the game's project: " << opened << '\n';
        return 1;
    }
    if (!manifest->scripts.empty() && !engine.scripts().adopt_prebuilt(folder / manifest->scripts, error)) {
        std::cerr << "cannot load the game's scripts: " << error << '\n';
        return 1;
    }

    relay::VulkanWindow window(manifest->name, manifest->width, manifest->height, engine.assets(), false);
    if (!window.valid()) {
        std::cerr << "cannot create the game window: " << window.error() << '\n';
        return 1;
    }
    if (manifest->icon) (void)window.set_icon_png(folder / "icon.png");
    window.set_window_mode(manifest->window_mode);
    relay_apps::connect_game_ui(window, engine);
    double particle_alpha = 1.0;
    relay_apps::connect_particles(window, engine, &particle_alpha);

    if (!engine.run_game()) {
        std::cerr << "cannot start the game: " << engine.run_game_error() << '\n';
        return 1;
    }
    std::uint64_t log_cursor = 0;
    print_logs(engine, log_cursor, debug);
    if (debug) std::cerr << "Playing " << manifest->name << ' ' << manifest->version << " on " << window.device_name() << '\n';

    using namespace std::chrono_literals;
    auto previous_frame = std::chrono::steady_clock::now();
    double unsimulated_seconds = 0.0;
    const auto shader_clock_start = previous_frame;
    std::chrono::steady_clock::duration limiter_period{};
    std::chrono::steady_clock::time_point limiter_deadline{};
    bool pointer_locked = false;
    std::uint32_t frames = 0;
    const auto run_start = std::chrono::steady_clock::now();
    while (engine.status().running) {
        const auto frame_start = std::chrono::steady_clock::now();
        unsimulated_seconds += std::min(std::chrono::duration<double>(frame_start - previous_frame).count(), 0.25);
        previous_frame = frame_start;
        if (window.poll_quit()) break;
        for (auto& input : window.drain_input_events()) engine.apply_input_event(std::move(input));
        if (!engine.status().running) break;

        const double step = engine.fixed_delta_seconds();
        for (int steps = 0; unsimulated_seconds >= step; ++steps) {
            // Drop time rather than fall ever further behind after a long frame.
            if (steps == 4) {
                unsimulated_seconds = 0.0;
                break;
            }
            engine.tick();
            unsimulated_seconds -= step;
        }
        if (const bool locked = engine.input().mouse_locked(); locked != pointer_locked) {
            window.set_pointer_locked(locked);
            pointer_locked = locked;
        }
        relay_apps::apply_graphics_settings(window, engine);
        window.set_shader_time(std::chrono::duration<double>(std::chrono::steady_clock::now() - shader_clock_start).count());
        window.set_render_interpolation(engine.render_interpolation(unsimulated_seconds / step));
        particle_alpha = unsimulated_seconds / step;
        if (!window.draw(engine.scene(), engine.status().elapsed_seconds)) {
            std::cerr << "drawing failed: " << window.error() << '\n';
            print_logs(engine, log_cursor, true);
            return 1;
        }
        engine.record_render_performance(window.gpu_frame_milliseconds(), window.draw_call_count(),
                                         window.render_resource_count());
        print_logs(engine, log_cursor, debug);
        ++frames;
        // Once the renderer has settled, say what it is really doing: which effects run and how
        // frames reach the display, so a slow game can be told from a slow setting.
        if (frames == 30U && (debug || frame_limit != 0U))
            std::cerr << "Renderer: " << window.lighting_status_json() << '\n'
                      << "Project graphics: vsync " << (engine.graphics_settings().vsync ? "on" : "off")
                      << ", frame rate limit " << engine.graphics_settings().frame_rate_limit << '\n';
        if (frame_limit != 0U && frames >= frame_limit) break;

        const auto limit = engine.graphics_settings().frame_rate_limit;
        if (limit != 0U && window.presented()) {
            const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / limit));
            const auto now = std::chrono::steady_clock::now();
            limiter_deadline = limiter_period == period && limiter_deadline + period >= now
                                   ? limiter_deadline + period
                                   : frame_start + period;
            limiter_period = period;
            std::this_thread::sleep_until(limiter_deadline);
        } else if (!window.presented()) {
            limiter_period = {};
            std::this_thread::sleep_until(frame_start + 4ms); // Nothing else paces a minimized window.
        } else {
            limiter_period = {};
        }
    }
    if (frame_limit != 0U) {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - run_start).count();
        std::cerr << frames << " frames in " << seconds << " s: " << (seconds > 0.0 ? frames / seconds : 0.0) << " FPS\n";
    }
    if (!capture_path.empty() &&
        !window.capture_image(capture_path, engine.scene(), engine.status().elapsed_seconds)) {
        std::cerr << "cannot capture a frame: " << window.error() << '\n';
        return 1;
    }
    engine.request_shutdown();
    print_logs(engine, log_cursor, debug);
    return 0;
}
