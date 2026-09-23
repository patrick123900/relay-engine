#include "relay/control/control_protocol.hpp"
#include "relay/control/generated_protocol.hpp"
#include "relay/control/local_server.hpp"
#include "relay/core/engine.hpp"
#include "relay/render/vulkan_device.hpp"

#ifdef RELAY_HAS_SDL3
#include "relay/platform/sdl_window.hpp"
#endif
#ifdef RELAY_HAS_VULKAN_WINDOW
#include "relay/platform/vulkan_window.hpp"
#endif
#ifdef RELAY_HAS_EDITOR_UI
#include "relay/editor/editor_ui.hpp"
#endif

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

int run_agent_mode() {
    relay::Engine engine;
    engine.pause();
    relay::ControlProtocol protocol(engine);
    protocol.configure_agent_from_environment();

    std::cout << "{\"event\":\"relay.ready\",\"protocol\":"
              << relay::protocol_schema_version << '}' << std::endl;
    std::string request;
    while (engine.status().running && std::getline(std::cin, request)) {
        if (!request.empty()) {
            std::cout << protocol.handle_agent(request) << std::endl;
        }
    }
    return 0;
}

int run_headless_demo() {
    relay::Engine engine;
    for (int frame = 0; frame < 120; ++frame) engine.tick();
    std::string error;
    if (!engine.capture("captures/headless-demo.bmp", error)) {
        std::cerr << "Capture failed: " << error << '\n';
        return 1;
    }
    std::cout << "Rendered 120 deterministic frames to captures/headless-demo.bmp\n";
    return 0;
}

int run_vulkan_probe() {
    relay::Engine engine;
    relay::ControlProtocol protocol(engine);
    const auto response = protocol.handle(R"({"id":1,"method":"render.capabilities"})");
    std::cout << response << '\n';
    return response.find(R"("logical_device_created":true)") != std::string::npos ? 0 : 1;
}

int run_socket_mode(const std::string_view port_text) {
    std::uint16_t port = 7777;
    if (!port_text.empty()) {
        unsigned int parsed_port = 0;
        const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed_port);
        if (parsed.ec != std::errc{} || parsed_port == 0 || parsed_port > 65535) {
            std::cerr << "Port must be between 1 and 65535\n";
            return 2;
        }
        port = static_cast<std::uint16_t>(parsed_port);
    }
    relay::Engine engine;
    std::string error;
    std::cout << "Relay control listening on 127.0.0.1:" << port << '\n';
    if (!relay::serve_local_control(engine, port, error)) {
        std::cerr << "Local control server failed: " << error << '\n';
        return 1;
    }
    return 0;
}

#ifdef RELAY_HAS_VULKAN_WINDOW
int run_windowed() {
    relay::Engine engine;
    relay::VulkanWindow window("Relay Engine — Vulkan First Light", 1280, 720, engine.assets());
    if (!window.valid()) {
        std::cerr << "Could not create the Vulkan window: " << window.error() << '\n';
        return 1;
    }
    std::cout << "Rendering with Vulkan on " << window.device_name() << '\n';

    using namespace std::chrono_literals;
    while (engine.status().running && !window.poll_quit()) {
        for (auto& input : window.drain_input_events()) engine.apply_input_event(std::move(input));
        const auto frame_start = std::chrono::steady_clock::now();
        engine.tick();
        if (!window.draw(engine.scene(), engine.status().elapsed_seconds)) {
            std::cerr << "Vulkan rendering failed: " << window.error() << '\n';
            return 1;
        }
        engine.record_render_performance(window.gpu_frame_milliseconds(), window.draw_call_count(),
                                         window.render_resource_count());
        std::this_thread::sleep_until(frame_start + 16ms);
    }
    return 0;
}

int run_vulkan_smoke() {
    const relay::AssetRegistry assets;
    relay::VulkanWindow window("Relay Vulkan Smoke Test", 640, 360, assets);
    if (!window.valid()) {
        std::cerr << "Vulkan smoke test initialization failed: " << window.error() << '\n';
        return 1;
    }
    for (std::uint32_t frame = 0; frame < 12; ++frame) {
        if (!window.draw(static_cast<double>(frame) / 60.0)) {
            std::cerr << "Vulkan smoke test draw failed: " << window.error() << '\n';
            return 1;
        }
    }
    window.resize(960, 540);
    for (std::uint32_t frame = 12; frame < 24; ++frame) {
        (void)window.poll_quit();
        if (!window.draw(static_cast<double>(frame) / 60.0)) {
            std::cerr << "Vulkan smoke test resize failed: " << window.error() << '\n';
            return 1;
        }
    }
    std::cout << "Vulkan smoke test passed on " << window.device_name() << '\n';
    return 0;
}

int run_vulkan_capture(const std::string_view path_text) {
    const std::string capture_path = path_text.empty() ? "captures/vulkan-frame.bmp"
                                                        : std::string(path_text);
    const relay::AssetRegistry assets;
    relay::VulkanWindow window("Relay Vulkan Capture", 1280, 720, assets);
    if (!window.valid()) {
        std::cerr << "Vulkan capture initialization failed: " << window.error() << '\n';
        return 1;
    }
    if (!window.draw(0.0) || !window.draw(1.0 / 60.0) ||
        !window.capture_image(capture_path, 2.0 / 60.0)) {
        std::cerr << "Vulkan capture failed: " << window.error() << '\n';
        return 1;
    }
    std::cout << "Captured Vulkan frame to " << capture_path << '\n';
    return 0;
}

int run_vulkan_model_smoke(const std::string_view filename) {
    relay::Engine engine;
    std::string error;
    const auto imported =
        relay::import_model_asset("assets", filename, engine.assets(), &engine.scene(), error);
    if (!imported.imported) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << imported.json() << '\n' << engine.assets().to_json() << '\n';
    const auto root = imported.roots.front();
    auto animator = *engine.scene().get(root)->animator;
    const auto *model = engine.assets().find_model(animator.model);
    animator.playing = model && !model->clips.empty();
    (void)engine.scene().set_animator(root, animator);
    relay::VulkanWindow window("Relay Phase D Model Smoke", 640, 360, engine.assets());
    if (!window.valid()) {
        std::cerr << window.error() << '\n';
        return 1;
    }
    for (unsigned frame = 0; frame < 90; ++frame) {
        if (frame == 45)
            window.resize(960, 540);
        (void)window.poll_quit();
        engine.step();
        if (!window.draw(engine.scene(), engine.status().elapsed_seconds)) {
            std::cerr << window.error() << '\n';
            return 1;
        }
    }
    animator = *engine.scene().get(root)->animator;
    animator.time_seconds =
        model && !model->clips.empty() ? model->clips.front().duration_seconds * 0.5 : 0.0;
    (void)engine.scene().set_animator(root, animator);
    if (!window.capture_image("/tmp/relay-phase-d-smoke.png", engine.scene(),
                              engine.status().elapsed_seconds)) {
        std::cerr << window.error() << '\n';
        return 1;
    }
    std::cout << "Phase D Vulkan model smoke passed on " << window.device_name() << '\n';
    return 0;
}

int run_vulkan_async_smoke(const std::string& filename) {
    relay::Engine engine;
    std::string error;
    const auto imported = relay::import_model_asset("assets", filename, engine.assets(), &engine.scene(), error);
    if (!imported.imported) { std::cerr << error << '\n'; return 1; }
    auto animator = *engine.scene().get(imported.roots.front())->animator;
    animator.playing = true;
    (void)engine.scene().set_animator(imported.roots.front(), animator);
    relay::VulkanWindow window("Relay Async Capture Smoke", 640, 360, engine.assets());
    if (!window.valid()) { std::cerr << window.error() << '\n'; return 1; }
    engine.set_gpu_capture_source([&](relay::Engine::FrameReceiver receiver, std::string& failure) {
        return window.readback_async(engine.scene(), engine.status().elapsed_seconds, std::move(receiver), failure);
    }, [&] { window.flush_readbacks(); });
    auto cleanup = std::unique_ptr<relay::Engine, std::function<void(relay::Engine*)>>(&engine, [](auto* runtime) {
        runtime->request_shutdown(); runtime->set_gpu_capture_source({}, {});
    });
    if (!engine.start_video("/tmp/relay-phase-e.webm", 30, 120, error, "vulkan")) { std::cerr << error << '\n'; return 1; }
    std::uint64_t job = 0;
    for (unsigned frame = 0; frame < 120; ++frame) {
        if (frame == 60) window.resize(960, 540);
        (void)window.poll_quit();
        engine.step();
        if (!window.draw(engine.scene(), engine.status().elapsed_seconds)) { std::cerr << window.error() << '\n'; return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        if (frame >= 90 && (!job || engine.capture_status(job).state == relay::CaptureJobState::failed)) job = engine.capture_async("/tmp/relay-phase-e.png", error, "vulkan");
    }
    if (!engine.stop_video(error)) { std::cerr << error << '\n'; return 1; }
    for (unsigned attempt = 0; attempt < 2000 && engine.video_status().finalizing; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (engine.video_status().finalizing || !engine.video_status().error.empty()) { std::cerr << "WebM finalization failed: " << engine.video_status().error << '\n'; return 1; }
    for (unsigned attempt = 0; job && attempt < 500 && engine.capture_status(job).state != relay::CaptureJobState::complete; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!job || engine.capture_status(job).state != relay::CaptureJobState::complete) { std::cerr << "async image failed: " << (job ? engine.capture_status(job).error : error) << '\n'; return 1; }
    std::cout << "Async Vulkan playback/resize PNG/WebM passed on " << window.device_name()
              << "; source=vulkan; frames=" << engine.video_status().submitted_frames
              << "; drops=" << engine.video_status().dropped_frames << '\n';
    return 0;
}

struct LiveInputState {
    std::mutex mutex;
    std::deque<std::string> requests;
    std::atomic<bool> reached_eof{false};
};

// One live-editor loop serves three modes. `read_stdin` drives the agent transport; `with_ui`
// installs the human editor. They are independent because a human and an agent are expected to
// operate the same runtime, and because both paths must issue identical ControlProtocol requests.
int run_live_editor_session(const bool with_ui, const bool read_stdin, bool& reader_detached) {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::VulkanWindow window(with_ui ? "Relay Editor" : "Relay Live Editor", 1280, 720,
                               engine.assets(), with_ui);
    if (!window.valid()) {
        std::cerr << "Live editor initialization failed: " << window.error() << '\n';
        return 1;
    }
    engine.set_gpu_capture_source([&](relay::Engine::FrameReceiver receiver, std::string& error) {
        return window.readback_async(engine.scene(), engine.status().elapsed_seconds, std::move(receiver), error);
    }, [&] { window.flush_readbacks(); });
    relay::ControlProtocol protocol(
        engine,
        [&](const std::filesystem::path& path, std::string& error) {
            if (!window.capture_image(path, engine.scene(), engine.status().elapsed_seconds)) {
                error = window.error();
                return false;
            }
            return true;
        },
        [&](const std::string_view kind) {
            if (kind == "graph") return window.render_graph_json();
            if (kind == "shader_interfaces") return window.shader_interfaces_json();
            if (kind == "upload_status") return window.upload_status_json();
            return std::string{};
        });

    protocol.configure_agent_from_environment();

#ifdef RELAY_HAS_EDITOR_UI
    // Trusted UI dispatch and permissioned agent dispatch share native operations and history.
    std::unique_ptr<relay::EditorUi> editor;
    if (with_ui) {
        editor = std::make_unique<relay::EditorUi>(
            [&protocol](const std::string_view request) { return protocol.handle(request); });
        protocol.set_editor_camera_handler([&editor](std::string_view request) { return editor->handle_camera_request(request); });
        editor->set_panel_visible("Agent", true);
        window.set_overlay(editor.get());
    }
#else
    if (with_ui) {
        std::cerr << "Relay was built without the editor interface (RELAY_ENABLE_EDITOR_UI)\n";
        return 1;
    }
#endif

    auto input_state = std::make_shared<LiveInputState>();
    std::optional<std::thread> input_reader;
    if (read_stdin) {
        input_reader.emplace([input_state] {
            std::string request;
            while (std::getline(std::cin, request)) {
                if (request.empty()) continue;
                std::scoped_lock lock(input_state->mutex);
                input_state->requests.push_back(std::move(request));
            }
            input_state->reached_eof.store(true);
        });
        std::cout << "{\"event\":\"relay.ready\",\"protocol\":" << relay::protocol_schema_version
                  << ",\"renderer\":\"vulkan\",\"device\":\"" << window.device_name()
                  << "\"}" << std::endl;
    } else {
        std::cerr << "Relay editor running on " << window.device_name() << '\n';
    }
    using namespace std::chrono_literals;
    bool window_closed = false;
    while (engine.status().running && !window_closed) {
        const auto frame_start = std::chrono::steady_clock::now();
        window_closed = window.poll_quit();
        for (auto& input : window.drain_input_events()) engine.apply_input_event(std::move(input));

        std::deque<std::string> requests;
        {
            std::scoped_lock lock(input_state->mutex);
            requests.swap(input_state->requests);
        }
        for (const auto& request : requests) {
            std::cout << protocol.handle_agent(request) << std::endl;
        }
        if (!engine.status().running || window_closed) break;

        if (read_stdin) {
            bool queue_empty = false;
            {
                std::scoped_lock lock(input_state->mutex);
                queue_empty = input_state->requests.empty();
            }
            if (input_state->reached_eof.load() && queue_empty) break;
        }

        engine.tick();
        if (!window.draw(engine.scene(), engine.status().elapsed_seconds)) {
            std::cerr << "Live editor Vulkan draw failed: " << window.error() << '\n';
            engine.request_shutdown();
            break;
        }
#ifdef RELAY_HAS_EDITOR_UI
        if (editor) editor->process_actions();
#endif
        engine.record_render_performance(window.gpu_frame_milliseconds(), window.draw_call_count(),
                                         window.render_resource_count());
        std::this_thread::sleep_until(frame_start + 16ms);
    }

    engine.request_shutdown();
    engine.set_gpu_capture_source({}, {});
#ifdef RELAY_HAS_EDITOR_UI
    // Retire the overlay's device objects while the window, and therefore the device, is still alive.
    window.set_overlay(nullptr);
    editor.reset();
#endif
    if (input_reader) {
        if (input_state->reached_eof.load()) {
            input_reader->join();
        } else {
            input_reader->detach();
            reader_detached = true;
        }
    }
    return window_closed ? 0 : (window.error().empty() ? 0 : 1);
}

int run_live_editor(const bool with_ui, const bool read_stdin) {
    bool reader_detached = false;
    const int exit_code = run_live_editor_session(with_ui, read_stdin, reader_detached);
    // runtime.quit and closing the window must end the process even while a caller still holds the
    // standard input pipe open. There is no portable way to interrupt a thread already blocked in
    // getline, so once the session scope above has destroyed the window, the engine and the editor,
    // leave immediately rather than letting exit handlers race that reader.
    if (reader_detached) {
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(exit_code);
    }
    return exit_code;
}
#elif defined(RELAY_HAS_SDL3)
int run_windowed() {
    relay::Engine engine;
    relay::SdlWindow window("Relay Engine — CPU fallback", 1280, 720);
    if (!window.valid()) {
        std::cerr << "Could not create a window: " << window.error() << '\n';
        return 1;
    }
    using namespace std::chrono_literals;
    while (engine.status().running && !window.poll_quit()) {
        for (auto& input : window.drain_input_events()) engine.apply_input_event(std::move(input));
        const auto frame_start = std::chrono::steady_clock::now();
        engine.tick();
        window.present(engine.frame());
        std::this_thread::sleep_until(frame_start + 16ms);
    }
    return 0;
}
#endif

} // namespace

// The public editor entry point starts the external provider bridge before any window initialization.
int run_editor_bridge(const char* executable) {
    const auto binary = std::filesystem::absolute(executable).string();
    const auto entry = (std::filesystem::path(RELAY_SOURCE_ROOT) / "tools/mcp-bridge/dist/index.js").string();
    if (!std::filesystem::is_regular_file(entry)) {
        std::cerr << "Build the external agent bridge before starting the editor\n";
        return 1;
    }
    const char* node = std::getenv("RELAY_NODE_EXECUTABLE");
    if (!node || !*node) node = "node";
#ifdef _WIN32
    const auto result = _spawnlp(_P_WAIT, node, node, entry.c_str(), "--editor", "--engine-binary", binary.c_str(), nullptr);
    return result < 0 ? 1 : static_cast<int>(result);
#else
    execlp(node, node, entry.c_str(), "--editor", "--engine-binary", binary.c_str(), static_cast<char*>(nullptr));
    std::cerr << "Could not start Node.js for the editor agent bridge\n";
    return 1;
#endif
}

int main(const int argument_count, char** arguments) {
    const std::string_view mode = argument_count > 1 ? arguments[1] : "";
    if (mode == "--agent-stdio") return run_agent_mode();
    if (mode == "--agent-port") {
        return run_socket_mode(argument_count > 2 ? std::string_view(arguments[2]) : std::string_view{});
    }
    if (mode == "--probe-vulkan") return run_vulkan_probe();
    if (mode == "--headless") return run_headless_demo();
#ifdef RELAY_HAS_VULKAN_WINDOW
    if (mode == "--vulkan-smoke") return run_vulkan_smoke();
    if (mode == "--vulkan-async-smoke") return run_vulkan_async_smoke(argument_count > 2 ? arguments[2] : "relay-dynamic-golden.gltf");
    if (mode == "--vulkan-model-smoke")
        return run_vulkan_model_smoke(argument_count > 2 ? arguments[2]
                                                         : "relay-dynamic-golden.gltf");
    if (mode == "--vulkan-capture") {
        return run_vulkan_capture(argument_count > 2 ? std::string_view(arguments[2])
                                                     : std::string_view{});
    }
    if (mode == "--editor-stdio") return run_live_editor(false, true);
    if (mode == "--editor") return run_editor_bridge(arguments[0]);
    if (mode == "--editor-ui-stdio") return run_live_editor(true, true);
#else
    if (mode == "--editor-stdio" || mode == "--editor" || mode == "--editor-ui-stdio") {
        std::cerr << "Relay was built without the Vulkan live editor\n";
        return 1;
    }
#endif
#ifdef RELAY_HAS_SDL3
    return run_windowed();
#else
    std::cerr << "Relay was built without SDL3; using the headless demo.\n";
    return run_headless_demo();
#endif
}
