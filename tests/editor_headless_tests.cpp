#include "relay/control/control_protocol.hpp"
#include "relay/core/engine.hpp"
#include "relay/editor/editor_ui.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void frame(relay::EditorUi& ui, int frames = 1) {
    for (int i = 0; i < frames; ++i) ui.build(1920, 1080);
}
void key(relay::EditorUi& ui, ImGuiKey keycode, bool ctrl = true, bool shift = false) {
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddKeyEvent(keycode, true);
    frame(ui, 2);
    io.AddKeyEvent(keycode, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    frame(ui, 2);
}
void click(relay::EditorUi& ui, const std::array<float, 4>& rect, bool ctrl = false, bool shift = false) {
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddMousePosEvent(rect[0] + 60, (rect[1] + rect[3]) / 2);
    frame(ui);
    io.AddMouseButtonEvent(0, true);
    frame(ui);
    io.AddMouseButtonEvent(0, false);
    frame(ui, 2);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    frame(ui);
}

void run() {
    relay::Engine engine;
    engine.scene().clear();
    engine.pause();
    relay::ControlProtocol protocol(engine);
    relay::ModelAsset model;
    model.name = "headless.model";
    model.nodes = {"Root"};
    relay::AnimationClip clip;
    clip.name = "Move";
    clip.duration_seconds = 2;
    model.clips = {clip};
    check(engine.assets().register_model(model), "register headless clip");
    const auto first = engine.scene().create("First");
    const auto second = engine.scene().create("Second");
    const auto third = engine.scene().create("Third");
    check(engine.scene().set_animator(first, relay::Animator{model.name}) &&
              engine.scene().set_animator(second, relay::Animator{model.name}), "attach animators");
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "CPU-only editor initializes");
    frame(ui, 5);
    const auto row = [&](relay::Entity entity) {
        const auto rect = ui.headless_item_rect("entity:" + entity.to_string());
        check(rect.has_value(), "visible hierarchy row is recorded");
        return *rect;
    };
    click(ui, row(first));
    click(ui, row(third), false, true);
    check(ui.selected_entities().size() == 3, "Shift-click selects visible hierarchy range");
    click(ui, row(third), true);
    check(ui.selected_entities().size() == 2, "Ctrl-click toggles selection without losing others");
    key(ui, ImGuiKey_C);
    check(engine.clipboard().nodes.size() == 2, "copy shortcut reaches shared clipboard");
    const auto depth = engine.scene_history().undo_depth();
    key(ui, ImGuiKey_X);
    check(engine.scene().entities().size() == 1 && engine.scene_history().undo_depth() == depth + 1,
          "cut shortcut removes group in one edit");
    key(ui, ImGuiKey_V);
    check(engine.scene().entities().size() == 3 && ui.selected_entities().size() == 2,
          "paste shortcut restores content and selects both copies");
    key(ui, ImGuiKey_Z);
    key(ui, ImGuiKey_Z);
    check(engine.scene().contains(first) && engine.scene().contains(second), "shortcut undo restores originals");
    key(ui, ImGuiKey_A);
    check(ui.selected_entities().size() == 3, "select-all shortcut includes every entity");
    check(!ImGui::FindWindowByName("History") && !ImGui::FindWindowByName("Project"),
          "history and project windows are absent by default");
    check(!ui.headless_item_rect("timeline:ruler"), "timeline is hidden by default");
    ui.set_panel_visible("Timeline", true);
    frame(ui, 3);
    ImGui::SetWindowFocus("Timeline");
    frame(ui, 3);
    const auto ruler = ui.headless_item_rect("timeline:ruler");
    check(ruler.has_value(), "timeline draws in the background with no window backend");
    auto& io = ImGui::GetIO();
    const auto x = [&](float fraction) { return (*ruler)[0] + ((*ruler)[2] - (*ruler)[0]) * fraction; };
    const auto y = ((*ruler)[1] + (*ruler)[3]) / 2;
    io.AddMousePosEvent(x(.25F), y);
    frame(ui);
    io.AddMouseButtonEvent(0, true);
    frame(ui);
    const auto scrub_depth = engine.scene_history().undo_depth();
    check(engine.scene().get(first)->animator->time_seconds > 0 &&
              engine.scene().get(first)->animator->time_seconds == engine.scene().get(second)->animator->time_seconds,
          "held timeline ruler applies time to both selected tracks before release");
    const auto before = engine.scene().get(first)->animator->time_seconds;
    io.AddMousePosEvent(x(.75F), y);
    frame(ui);
    check(engine.scene().get(first)->animator->time_seconds > before &&
              engine.scene_history().undo_depth() == scrub_depth,
          "moving held ruler changes pose without adding history");
    io.AddMouseButtonEvent(0, false);
    frame(ui, 2);
    key(ui, ImGuiKey_Z);
    check(engine.scene().get(first)->animator->time_seconds == 0 &&
              engine.scene().get(second)->animator->time_seconds == 0, "one shortcut undo rewinds entire timeline scrub");
    (void)protocol.handle(R"({"id":1,"method":"scene.create","name":"Unsaved"})");
    ui.invalidate();
    // A synthetic SDL quit event is passed directly to the UI, never to the OS event queue.
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    check(ui.handle_event(&quit), "editor intercepts native close");
    frame(ui, 2);
    check(engine.status().running && ImGui::FindWindowByName("Unsaved changes"),
          "native quit shows unsaved guard instead of shutting down");
    key(ui, ImGuiKey_Escape, false);
    check(engine.status().running, "cancel close keeps runtime alive");
    std::cout << "Headless editor input tests passed without SDL windows or OS input\n";
}
void project_ui() {
    relay::Engine engine;
    relay::ControlProtocol protocol(engine);
    const auto created = protocol.handle(R"({"id":1,"method":"project.create","filename":"projects/headless/project.relayproject","name":"Headless"})");
    check(created.find("\"ok\":true") != std::string::npos, "create background project fixture");
    (void)protocol.handle(R"({"id":2,"method":"scene.create","name":"Project scene"})");
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize project editor without windows");
    frame(ui, 5);
    key(ui, ImGuiKey_S);
    // Save As takes keyboard focus; submit its default safe filename entirely through ImGui IO.
    key(ui, ImGuiKey_Enter, false);
    check(std::filesystem::exists("projects/headless/scenes/main.relay.json") && engine.project()->scenes.size() == 1 &&
              engine.project()->startup_scene == "main.relay.json",
          "editor Save As writes scene and registers project startup membership");
    key(ui, ImGuiKey_N);
    check(engine.scene().entities().empty(), "new scene after save does not show a discard prompt");
    check(!ui.headless_item_rect("project:scene:main.relay.json"), "project panel is hidden by default");
    ui.set_panel_visible("Project", true);
    frame(ui, 3);
    ImGui::SetWindowFocus("Project");
    frame(ui, 3);
    const auto member = ui.headless_item_rect("project:scene:main.relay.json");
    check(member.has_value(), "project browser shows saved scene membership");
    click(ui, *member);
    check(engine.scene().entities().size() == 1 &&
              engine.scene().get(engine.scene().entities().front())->name == "Project scene",
          "project browser opens saved scene through guarded protocol workflow");
    std::cout << "Headless project save/browser tests passed\n";
}
} // namespace

int main() {
    const auto original = std::filesystem::current_path();
    const auto temporary = std::filesystem::temp_directory_path() /
        ("relay-headless-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(temporary);
    std::filesystem::current_path(temporary);
    int result = 0;
    try { run(); project_ui(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    std::filesystem::current_path(original);
    std::filesystem::remove_all(temporary);
    return result;
}
