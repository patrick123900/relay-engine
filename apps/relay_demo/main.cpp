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

#include <chrono>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

int run_agent_mode() {
    relay::Engine engine;
    engine.pause();
    relay::ControlProtocol protocol(engine);

    std::cout << "{\"event\":\"relay.ready\",\"protocol\":"
              << relay::protocol_schema_version << '}' << std::endl;
    std::string request;
    while (engine.status().running && std::getline(std::cin, request)) {
        if (!request.empty()) {
            std::cout << protocol.handle(request) << std::endl;
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
    relay::VulkanWindow window("Relay Engine — Vulkan First Light", 1280, 720);
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
    relay::VulkanWindow window("Relay Vulkan Smoke Test", 640, 360);
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
    relay::VulkanWindow window("Relay Vulkan Capture", 1280, 720);
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

struct LiveInputState {
    std::mutex mutex;
    std::deque<std::string> requests;
    std::atomic<bool> reached_eof{false};
};

int run_live_editor_stdio() {
    relay::Engine engine;
    relay::VulkanWindow window("Relay Live Editor", 1280, 720);
    if (!window.valid()) {
        std::cerr << "Live editor initialization failed: " << window.error() << '\n';
        return 1;
    }
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
            return std::string{};
        });

    auto input_state = std::make_shared<LiveInputState>();
    std::thread input_reader([input_state] {
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
            std::cout << protocol.handle(request) << std::endl;
        }
        if (!engine.status().running || window_closed) break;

        bool queue_empty = false;
        {
            std::scoped_lock lock(input_state->mutex);
            queue_empty = input_state->requests.empty();
        }
        if (input_state->reached_eof.load() && queue_empty) break;

        engine.tick();
        if (!window.draw(engine.scene(), engine.status().elapsed_seconds)) {
            std::cerr << "Live editor Vulkan draw failed: " << window.error() << '\n';
            engine.request_shutdown();
            break;
        }
        engine.record_render_performance(window.gpu_frame_milliseconds(), window.draw_call_count(),
                                         window.render_resource_count());
        std::this_thread::sleep_until(frame_start + 16ms);
    }

    if (input_state->reached_eof.load()) input_reader.join();
    else input_reader.detach();
    return window_closed ? 0 : (window.error().empty() ? 0 : 1);
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
    if (mode == "--vulkan-capture") {
        return run_vulkan_capture(argument_count > 2 ? std::string_view(arguments[2])
                                                     : std::string_view{});
    }
    if (mode == "--editor-stdio") return run_live_editor_stdio();
#else
    if (mode == "--editor-stdio") {
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
