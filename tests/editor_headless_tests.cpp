#include "relay/control/control_protocol.hpp"
#include "relay/core/input.hpp"
#include "relay/core/json.hpp"
#include "relay/core/engine.hpp"
#include "relay/editor/editor_layout.hpp"
#include "relay/editor/editor_ui.hpp"
#include "relay/editor/chat_media.hpp"
#include "relay/editor/wrapped_input.hpp"
#include "relay/observe/profiler.hpp"
#include "relay/render/scene_render.hpp"
#include "relay/scene/project.hpp"
#include "relay/script/script_system.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <thread>

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
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    engine.scene().clear();
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
    check(engine.scene().set_collider(first, relay::BoxCollider{}), "attach headless box collider");
    check(engine.scene().set_animator(first, relay::Animator{model.name}) &&
              engine.scene().set_animator(second, relay::Animator{model.name}), "attach animators");
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "CPU-only editor initializes");
    frame(ui, 5);
    const auto* viewport_window = ImGui::FindWindowByName("Viewport");
    check(viewport_window, "headless viewport exists");
    const auto wire_color = IM_COL32(75, 224, 174, 190);
    const auto has_wireframe = [&] {
        for (const auto& vertex : viewport_window->DrawList->VtxBuffer)
            if (vertex.col == wire_color) return true;
        return false;
    };
    check(has_wireframe(), "editor viewport draws collider wireframe without a desktop");
    const auto wire_vertices = [&] {
        std::size_t count = 0;
        for (const auto& vertex : viewport_window->DrawList->VtxBuffer)
            count += vertex.col == wire_color;
        return count;
    };
    const auto box_vertices = wire_vertices();
    relay::BoxCollider round;
    round.type = relay::BoxCollider::Type::sphere;
    check(engine.scene().set_collider(third, round), "attach headless sphere collider");
    frame(ui, 40);
    // Three 32-segment great circles draw far more lines than a second 12-edge box would.
    check(wire_vertices() > box_vertices * 4U, "sphere collider draws a round outline");
    check(engine.scene().set_collider(third, std::nullopt), "remove headless sphere collider");
    frame(ui, 40);
    const auto row = [&](relay::Entity entity) {
        const auto rect = ui.headless_item_rect("entity:" + entity.to_string());
        check(rect.has_value(), "visible hierarchy row is recorded");
        return *rect;
    };
    click(ui, row(first));
    if (const auto extent = ui.headless_item_rect("collider:half_extent:Y")) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(((*extent)[0] + (*extent)[2]) * 0.5F,
                            ((*extent)[1] + (*extent)[3]) * 0.5F);
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        frame(ui);
        io.AddMouseButtonEvent(0, true);
        frame(ui);
        io.AddMouseButtonEvent(0, false);
        frame(ui);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddInputCharactersUTF8("0");
        frame(ui, 2);
        key(ui, ImGuiKey_Enter, false);
        frame(ui, 2);
        check(std::abs(engine.scene().get(first)->collider->half_extents.y - 0.01) < 1e-9,
              "zero entered in collider inspector commits a visible safe thickness");
    } else {
        check(false, "selected collider half-extent field is visible in headless inspector");
    }
    click(ui, row(third), false, true);
    check(ui.selected_entities().size() == 3, "Shift-click selects visible hierarchy range");
    check(std::abs(row(first)[3] - row(second)[1]) < .01F && std::abs(row(second)[3] - row(third)[1]) < .01F,
          "adjacent selected hierarchy rows have no vertical highlight gap");
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
    check(protocol.handle(R"({"id":1001,"method":"runtime.play"})").find("\"ok\":true") != std::string::npos,
          "headless editor can start a game session");
    frame(ui, 40);
    check(!has_wireframe(), "game viewport does not show editor collider wireframes");
    check(!ui.view_override() && !ui.ground_grid_visible() && ui.selected_entities().empty(),
          "game viewport uses scene camera and hides editor overlays");
    check(protocol.handle(R"({"id":1002,"method":"runtime.stop"})").find("\"ok\":true") != std::string::npos,
          "headless editor can stop the game session");
    frame(ui, 40);
    check(ui.view_override() && ui.ground_grid_visible(),
          "stopping restores the editor inspection view");
    const auto camera_entity = engine.scene().create("Editor camera marker");
    const auto directional = engine.scene().create("Directional marker");
    const auto point = engine.scene().create("Point marker");
    const auto spot = engine.scene().create("Spot marker");
    relay::Light directional_light;
    directional_light.type = relay::Light::Type::directional;
    relay::Light point_light;
    point_light.type = relay::Light::Type::point;
    relay::Light spot_light;
    spot_light.type = relay::Light::Type::spot;
    check(engine.scene().set_camera(camera_entity, relay::Camera{}) &&
              engine.scene().set_light(directional, directional_light) &&
              engine.scene().set_light(point, point_light) &&
              engine.scene().set_light(spot, spot_light) &&
              engine.scene().set_transform(directional,
                  relay::Transform{{-2, 0, 0}, {}, {1, 1, 1}}) &&
              engine.scene().set_transform(point,
                  relay::Transform{{0, 2, 0}, {}, {1, 1, 1}}) &&
              engine.scene().set_transform(spot,
                  relay::Transform{{2, 0, 0}, {}, {1, 1, 1}}),
          "camera and light marker fixtures are valid");
    frame(ui, 40);
    check(ui.headless_item_rect("node:" + camera_entity.to_string() + ":camera") &&
              ui.headless_item_rect("node:" + directional.to_string() + ":light") &&
              ui.headless_item_rect("node:" + point.to_string() + ":light") &&
              ui.headless_item_rect("node:" + spot.to_string() + ":light"),
          "editor viewport draws distinct camera and light node markers");
    const auto has_icon_color = [&](ImU32 color) {
        for (const auto& vertex : viewport_window->DrawList->VtxBuffer)
            if (vertex.col == color) return true;
        return false;
    };
    check(has_icon_color(IM_COL32(98, 204, 255, 255)) &&
              has_icon_color(IM_COL32(255, 219, 112, 255)) &&
              has_icon_color(IM_COL32(255, 235, 130, 255)) &&
              has_icon_color(IM_COL32(255, 161, 91, 255)),
          "camera, directional, point and spot markers use distinct viewport colors");
    const auto marker = *ui.headless_item_rect("node:" + camera_entity.to_string() + ":camera");
    auto& marker_io = ImGui::GetIO();
    marker_io.AddMousePosEvent((marker[0] + marker[2]) * 0.5F,
                               (marker[1] + marker[3]) * 0.5F);
    frame(ui);
    marker_io.AddMouseButtonEvent(0, true);
    frame(ui);
    marker_io.AddMouseButtonEvent(0, false);
    frame(ui, 2);
    check(!ui.selected_entities().empty() && ui.selected_entities().front() == camera_entity,
          "clicking an invisible node's camera icon selects it in the editor");
    const auto frustum_color = IM_COL32(98, 204, 255, 215);
    bool has_frustum = false;
    for (const auto& vertex : viewport_window->DrawList->VtxBuffer)
        has_frustum |= vertex.col == frustum_color;
    check(has_frustum, "selected camera draws its view wireframe in the editor viewport");
    relay::TransformAnimation camera_keys;
    camera_keys.duration_seconds = 2.0;
    camera_keys.keys = {{0.0, relay::Transform{}},
                        {2.0, relay::Transform{{2, 0, 0}, {}, {1, 1, 1}}}};
    check(engine.scene().set_transform_animation(camera_entity, camera_keys),
          "camera marker accepts authored transform keys");
    frame(ui, 40);
    const auto before_animation = ui.headless_item_rect("node:" + camera_entity.to_string() + ":camera");
    camera_keys.time_seconds = 1.0;
    check(engine.scene().set_transform_animation(camera_entity, camera_keys),
          "camera marker can scrub authored transform keys");
    frame(ui, 40);
    const auto after_animation = ui.headless_item_rect("node:" + camera_entity.to_string() + ":camera");
    check(before_animation && after_animation && (*after_animation)[0] > (*before_animation)[0] + 5.0F,
          "camera marker follows the sampled scene-owned transform track");
    check(protocol.handle(R"({"id":1003,"method":"runtime.play"})").find("\"ok\":true") != std::string::npos,
          "game session starts with node markers present");
    frame(ui, 40);
    check(!ui.headless_item_rect("node:" + camera_entity.to_string() + ":camera"),
          "game viewport hides editor-only node markers");
    check(protocol.handle(R"({"id":1004,"method":"runtime.stop"})").find("\"ok\":true") != std::string::npos,
          "game session with node markers stops");
    std::cout << "Headless editor input tests passed without SDL windows or OS input\n";
}
void project_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
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
void click_center(relay::EditorUi& ui, const std::array<float, 4>& rect) {
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
    frame(ui);
    io.AddMouseButtonEvent(0, true);
    frame(ui);
    io.AddMouseButtonEvent(0, false);
    frame(ui, 2);
}

void double_click(relay::EditorUi& ui, const std::array<float, 4>& rect) {
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(rect[0] + 60, (rect[1] + rect[3]) / 2);
    frame(ui);
    for (int press = 0; press < 2; ++press) {
        io.AddMouseButtonEvent(0, true);
        frame(ui);
        io.AddMouseButtonEvent(0, false);
        frame(ui);
    }
    frame(ui, 2);
}

void type_text(relay::EditorUi& ui, const char* text) {
    ImGui::GetIO().AddInputCharactersUTF8(text);
    frame(ui, 2);
}

void hierarchy_and_assets_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    check(protocol.handle(R"({"id":1,"method":"project.create","filename":"projects/browser/project.relayproject","name":"Browser"})")
              .find("\"ok\":true") != std::string::npos, "create asset browser project");
    std::filesystem::create_directories("projects/browser/models");
    std::ofstream("projects/browser/models/tri.obj") << "o tri\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    std::ofstream("projects/browser/readme.txt") << "readme";
    const auto alpha = engine.scene().create("Alpha");
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize browser editor without windows");
    frame(ui, 5);
    check(!ui.headless_item_rect("rename:entity"), "hierarchy has no standing create or rename field");

    const auto alpha_row = ui.headless_item_rect("entity:" + alpha.to_string());
    check(alpha_row.has_value(), "hierarchy shows the entity row");
    click(ui, *alpha_row);
    key(ui, ImGuiKey_F2, false);
    const auto field = ui.headless_item_rect("rename:entity");
    check(field.has_value() && std::abs((*field)[1] - (*alpha_row)[1]) < 2.0F,
          "F2 opens the rename field on the entity's own row");
    type_text(ui, "Renamed");
    key(ui, ImGuiKey_Enter, false);
    check(engine.scene().get(alpha)->name == "Renamed" && !ui.headless_item_rect("rename:entity"),
          "Enter commits the inline entity rename");
    key(ui, ImGuiKey_F2, false);
    type_text(ui, "Discarded");
    key(ui, ImGuiKey_Escape, false);
    check(engine.scene().get(alpha)->name == "Renamed", "Escape cancels the inline rename");
    // A second, slow click on the selected row renames it once the double-click time passes.
    click(ui, *ui.headless_item_rect("entity:" + alpha.to_string()));
    frame(ui, 30);
    check(ui.headless_item_rect("rename:entity").has_value(), "slow second click starts renaming");
    key(ui, ImGuiKey_Escape, false);
    double_click(ui, *ui.headless_item_rect("entity:" + alpha.to_string()));
    frame(ui, 30);
    check(!ui.headless_item_rect("rename:entity"), "a quick double click does not rename");

    frame(ui, 40);
    const auto models = ui.headless_item_rect("asset:models");
    check(models.has_value() && ui.headless_item_rect("asset:readme.txt") &&
              !ui.headless_item_rect("asset:project.relayproject"),
          "asset tree lists the project root folder without the project file");
    check(!ui.headless_item_rect("asset:models/tri.obj"), "folders start collapsed");
    double_click(ui, *models);
    const auto model = ui.headless_item_rect("asset:models/tri.obj");
    check(model.has_value() && ui.headless_item_rect("asset:readme.txt") &&
              (*model)[0] > (*models)[0] && (*model)[1] > (*models)[1],
          "double-clicking a folder expands it in place, indented below its row");
    const auto before = engine.scene().entities().size();
    // The model row sits where the folder was; wait out the double-click window first.
    frame(ui, 30);
    double_click(ui, *model);
    check(engine.scene().entities().size() > before, "double-clicking a model imports it into the scene");
    frame(ui, 30);
    click(ui, *ui.headless_item_rect("asset:models/tri.obj"));
    key(ui, ImGuiKey_F2, false);
    check(ui.headless_item_rect("rename:asset").has_value(), "F2 renames the selected asset in place");
    type_text(ui, "triangle.obj");
    key(ui, ImGuiKey_Enter, false);
    frame(ui, 3);
    check(std::filesystem::exists("projects/browser/models/triangle.obj") &&
              !std::filesystem::exists("projects/browser/models/tri.obj") &&
              ui.headless_item_rect("asset:models/triangle.obj"),
          "asset rename moves the file and keeps its folder expanded");
    check(!ui.headless_item_rect("assets:up"), "the tree has no parent-folder button");
    click(ui, *ui.headless_item_rect("assets:search"));
    type_text(ui, "TRI");
    check(ui.headless_item_rect("asset:models/triangle.obj").has_value() &&
              !ui.headless_item_rect("asset:models"),
          "typing in the asset search lists nested matches without expanding folders");
    key(ui, ImGuiKey_Escape, false);
    frame(ui, 2);
    check(ui.headless_item_rect("asset:models").has_value(), "Escape clears the search back to the tree");
    click(ui, *ui.headless_item_rect("assets:empty"));
    click_center(ui, *ui.headless_item_rect("assets:filter"));
    const auto models_filter = ui.headless_item_rect("assets:filter:model");
    check(models_filter.has_value(), "the filter button opens the category menu");
    click(ui, *models_filter);
    check(ui.headless_item_rect("assets:filter:scene").has_value() &&
              ui.headless_item_rect("asset:models/triangle.obj").has_value() &&
              !ui.headless_item_rect("asset:readme.txt"),
          "choosing a category filters the list and keeps the menu open");
    click(ui, *ui.headless_item_rect("assets:filter:model"));
    click(ui, *ui.headless_item_rect("assets:empty"));
    check(!ui.headless_item_rect("assets:filter:model") && ui.headless_item_rect("asset:models"),
          "clearing the only category and clicking away returns to the tree");
    std::vector<std::pair<std::filesystem::path, bool>> shown;
    ui.set_file_browser_handler([&](const std::filesystem::path& path, bool directory) {
        shown.emplace_back(path, directory);
    });
    {
        auto& io = ImGui::GetIO();
        const auto row = *ui.headless_item_rect("asset:models");
        io.AddMousePosEvent(row[0] + 60, (row[1] + row[3]) / 2);
        frame(ui);
        io.AddMouseButtonEvent(1, true);
        frame(ui);
        io.AddMouseButtonEvent(1, false);
        frame(ui, 2);
    }
    const auto open_item = ui.headless_item_rect("assets:menu:file_browser");
    check(open_item.has_value(), "the asset context menu offers Open in file browser");
    click(ui, *open_item);
    check(shown.size() == 1U && shown.front().second &&
              shown.front().first == std::filesystem::path("projects/browser") / "models",
          "Open in file browser opens the folder through the host file browser");
    frame(ui, 30);
    double_click(ui, *ui.headless_item_rect("asset:models"));
    check(!ui.headless_item_rect("asset:models/triangle.obj") &&
              ui.headless_item_rect("asset:models").has_value(),
          "double-clicking an expanded folder collapses it");
    {
        auto& io = ImGui::GetIO();
        const auto from = *ui.headless_item_rect("asset:readme.txt");
        const auto to = *ui.headless_item_rect("asset:models");
        io.AddMousePosEvent(from[0] + 60, (from[1] + from[3]) / 2);
        frame(ui);
        io.AddMouseButtonEvent(0, true);
        frame(ui);
        for (int step = 1; step <= 8; ++step) {
            io.AddMousePosEvent(from[0] + 60, from[1] + (to[1] - from[1]) * static_cast<float>(step) / 8.0F + 8);
            frame(ui);
        }
        io.AddMouseButtonEvent(0, false);
        frame(ui, 3);
    }
    check(std::filesystem::exists("projects/browser/models/readme.txt") &&
              ui.headless_item_rect("asset:models/readme.txt").has_value(),
          "dragging a file onto a folder moves it there and expands the folder");
    std::cout << "Headless hierarchy rename and asset browser tests passed\n";
}

void layout_persistence_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    std::string error;
    {
        relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
        check(ui.initialize_headless(error), "first layout session initializes");
        frame(ui, 3);
        check(!ui.panel_visible("History") && ui.panel_visible("Hierarchy"),
              "a fresh layout uses the default panels");
        ui.set_panel_visible("History", true);
        ui.set_panel_visible("Hierarchy", false);
        frame(ui, 3);
    }
    std::ifstream saved(".relay/headless-layout.ini");
    const std::string text{std::istreambuf_iterator<char>(saved), {}};
    check(text.find("[Relay][Preferences]") != std::string::npos &&
              text.find("panel.History=1") != std::string::npos &&
              text.find("panel.Hierarchy=0") != std::string::npos,
          "closing the editor saves panel visibility with the dock layout");
    {
        relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
        check(ui.initialize_headless(error), "second layout session initializes");
        ui.set_panel_visible("Hierarchy", true); // A host default applied before the first frame.
        frame(ui, 3);
        check(ui.panel_visible("History") && !ui.panel_visible("Hierarchy"),
              "reopening restores saved panels over host defaults");
    }
#if !defined(_WIN32) && !defined(__APPLE__)
    {
        // Without an override the layout lives in the user config directory, independent of the
        // build and working directory, and an older per-workspace layout is carried over once.
        const auto* previous = std::getenv("XDG_CONFIG_HOME");
        const std::string restore = previous ? previous : "";
        const auto config_home = std::filesystem::absolute("user-config");
        setenv("XDG_CONFIG_HOME", config_home.c_str(), 1);
        std::filesystem::create_directories(".relay");
        std::ofstream(".relay/editor-layout.ini") << "[Relay][Preferences]\npanel.History=1\n";
        const auto path = relay::default_editor_layout_path();
        check(std::filesystem::path(path) == config_home / "relay-engine" / "editor-layout.ini" &&
                  std::filesystem::exists(path),
              "the default layout path is per user and migrates the old workspace layout");
        std::ofstream(".relay/editor-layout.ini") << "stale";
        std::ifstream migrated(path);
        const std::string contents{std::istreambuf_iterator<char>(migrated), {}};
        check(relay::default_editor_layout_path() == path &&
                  contents.find("panel.History=1") != std::string::npos,
              "an existing user layout is never replaced by the old workspace copy");
        if (previous) setenv("XDG_CONFIG_HOME", restore.c_str(), 1);
        else unsetenv("XDG_CONFIG_HOME");
    }
#endif
    {
        // The selected tab in each shared dock node survives a restart.
        std::filesystem::remove(".relay/headless-layout.ini");
        const auto selected = [](const char* name) {
            const auto* window = ImGui::FindWindowByName(name);
            return window && window->DockNode && window->DockNode->TabBar &&
                   window->DockNode->TabBar->SelectedTabId == window->TabId;
        };
        {
            relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
            check(ui.initialize_headless(error), "tab session initializes");
            ui.set_panel_visible("Agent", true);
            ui.set_panel_visible("History", true);
            ui.set_panel_visible("Timeline", true);
            frame(ui, 3);
            ImGui::SetWindowFocus("Inspector");
            frame(ui, 2);
            ImGui::SetWindowFocus("History");
            frame(ui, 2);
            ImGui::SetWindowFocus("Viewport");
            frame(ui, 2);
            check(selected("Inspector") && selected("History"), "tabs are selected before closing");
        }
        relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
        check(ui.initialize_headless(error), "tab session reopens");
        ui.set_panel_visible("Agent", true);
        frame(ui, 5);
        check(selected("Inspector") && selected("History"),
              "reopening restores the selected tab in each dock node, even under the Agent default");
    }
    std::cout << "Headless layout persistence tests passed\n";
}

void agent_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    const auto target = engine.scene().create("Approved target");
    const auto other = engine.scene().create("Denied target");
    const auto request_text = "{\"id\":1,\"method\":\"session.request\",\"scope\":\"scene.set_transform\",\"kind\":\"entity\",\"target\":\"" + target.to_string() + "\"}";
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize approval UI without windows");
    check(protocol.handle(R"({"id":10,"method":"session.status"})").find("\"auto_approval\":true") != std::string::npos, "editor allows all actions by default");
    (void)protocol.handle(R"({"id":11,"method":"session.auto_approval","enabled":false})");
    check(protocol.handle_agent(request_text).find("\"ok\":true") != std::string::npos, "agent requests limited access");
    ui.set_panel_visible("Agent", true);
    frame(ui, 5);
    // ImGui-only focus/input queues; never sends OS input or changes desktop focus.
    ImGui::SetWindowFocus("Agent");
    frame(ui, 3);
    auto press = [&](const std::string& name) {
        auto rect = ui.headless_item_rect(name);
        check(rect.has_value(), ("agent control visible: " + name).c_str());
        const float center = ((*rect)[0] + (*rect)[2]) / 2;
        (*rect)[0] = center - 60;
        click(ui, *rect);
    };
    press("agent:access-tab");
    frame(ui, 3);
    const auto depth = engine.scene_history().undo_depth();
    press("agent:allow:1");
    check(engine.scene_history().undo_depth() == depth, "approval does not enter scene undo");
    auto transform = [&](relay::Entity entity) {
        return protocol.handle_agent("{\"id\":2,\"method\":\"scene.set_transform\",\"entity\":\"" + entity.to_string() + "\",\"px\":4}");
    };
    check(transform(target).find("\"ok\":true") != std::string::npos, "UI approval permits scoped native mutation");
    check(transform(other).find("\"ok\":false") != std::string::npos, "UI grant cannot edit another entity");
    press("agent:revoke");
    check(transform(target).find("\"ok\":false") != std::string::npos, "UI revocation denies subsequent actions");
    check(protocol.handle_agent("{\"id\":3,\"method\":\"session.request\",\"scope\":\"scene.clear\"}").find("\"ok\":true") != std::string::npos, "destructive request queues");
    frame(ui, 40);
    press("agent:deny:2");
    check(protocol.handle_agent("{\"id\":4,\"method\":\"scene.clear\"}").find("\"ok\":false") != std::string::npos, "UI denial leaves destructive action forbidden");
    press("agent:auto-approval");
    check(transform(other).find("\"ok\":true") != std::string::npos, "UI Auto approval permits previously denied entity");
    press("agent:revoke");
    check(transform(other).find("\"ok\":false") != std::string::npos, "Revoke all disables Auto approval");
    press("agent:actions-tab");
    frame(ui, 3);
    press("agent:export");
    check(std::filesystem::exists(".relay/audits/session-audit.jsonl"), "UI audit export routes through protocol");
    std::cout << "Headless agent approval/revocation/audit tests passed\n";
}

void chat_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    const auto revision = engine.scene_history().revision();
    const auto* configured = std::getenv("RELAY_BRIDGE_TOKEN");
    const auto previous = configured ? std::optional<std::string>{configured} : std::nullopt;
    const std::string token(64, 'c');
#ifdef _WIN32
    _putenv_s("RELAY_BRIDGE_TOKEN", token.c_str());
#else
    setenv("RELAY_BRIDGE_TOKEN", token.c_str(), 1);
#endif
    protocol.configure_agent_from_environment();
#ifdef _WIN32
    _putenv_s("RELAY_BRIDGE_TOKEN", previous ? previous->c_str() : "");
#else
    if (previous) setenv("RELAY_BRIDGE_TOKEN", previous->c_str(), 1);
    else unsetenv("RELAY_BRIDGE_TOKEN");
#endif
    const auto published = protocol.handle_agent("{\"id\":1,\"method\":\"bridge.publish\",\"bridge_token\":\"" + token +
        "\",\"view\":\"" + relay::json_escape(R"({"status":"Fixture bridge ready","busy":false,"messages":[],"provider":{"selected":"compatible"}})") + "\"}");
    check(published.find("\"ok\":true") != std::string::npos, "fixture bridge authenticates display projection");
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "chat input initializes without desktop");
    ui.set_panel_visible("Agent", true);
    frame(ui, 5);
    ImGui::SetWindowFocus("Agent");
    frame(ui, 3);
    const auto press = [&](const std::string& name) {
        auto rect = ui.headless_item_rect(name);
        check(rect.has_value(), ("chat control visible: " + name).c_str());
        auto* window = ImGui::FindWindowByName("Agent");
        if ((*rect)[1] < window->InnerClipRect.Min.y || (*rect)[3] > window->InnerClipRect.Max.y) {
            const auto desired = window->Scroll.y + ((*rect)[1] + (*rect)[3]) / 2 -
                (window->InnerClipRect.Min.y + window->InnerClipRect.Max.y) / 2;
            ImGui::SetScrollY(window, desired);
            frame(ui, 3);
            rect = ui.headless_item_rect(name);
            check(rect.has_value(), "chat control remains visible after ImGui-only scrolling");
        }
        (*rect)[0] = ((*rect)[0] + (*rect)[2]) / 2 - 60;
        click(ui, *rect);
    };
    const auto publish = [&](const std::string& fixture) {
        check(protocol.handle_agent("{\"id\":7,\"method\":\"bridge.publish\",\"bridge_token\":\"" + token + "\",\"view\":\"" + relay::json_escape(fixture) + "\"}").find("\"ok\":true") != std::string::npos, "OpenAI fixture projection accepted");
        frame(ui, 40);
    };
    const auto consume = [&]() { return protocol.handle_agent("{\"id\":8,\"method\":\"bridge.poll\",\"bridge_token\":\"" + token + "\"}"); };
    ui.set_attachment_picker([](auto complete) { complete({"/tmp/attachment one.txt", "/tmp/image.png"}); });
    press("agent:attach"); frame(ui, 3);
    check(ui.headless_item_rect("agent:attachment").has_value(), "picker results become removable chips without opening an OS dialog");
    press("agent:send");
    const auto attached_submission = consume();
    check(attached_submission.find("attachment one.txt") != std::string::npos && attached_submission.find("image.png") != std::string::npos, "attachment-only send carries selected paths through host mailbox");
    const auto drop_rect = ui.headless_item_rect("agent:message");
    SDL_Event drop{}; drop.type = SDL_EVENT_DROP_POSITION;
    drop.drop.x = ((*drop_rect)[0] + (*drop_rect)[2]) * .5F; drop.drop.y = ((*drop_rect)[1] + (*drop_rect)[3]) * .5F;
    check(ui.handle_event(&drop), "drop position inside composer accepted");
    drop.type = SDL_EVENT_DROP_FILE; drop.drop.data = "/tmp/dropped.txt"; ui.handle_event(&drop);
    drop.type = SDL_EVENT_DROP_COMPLETE; ui.handle_event(&drop); frame(ui, 3);
    press("agent:send"); check(consume().find("dropped.txt") != std::string::npos, "synthetic SDL file drop reaches bridge without desktop input");
    drop.type = SDL_EVENT_DROP_FILE; drop.drop.x = -500; drop.drop.data = "/tmp/rejected.txt"; ui.handle_event(&drop); frame(ui, 3);
    check(!ui.headless_item_rect("agent:attachment"), "file drop outside composer ignored");
    press("agent:message");
    ImGui::GetIO().AddInputCharactersUTF8("Hello bridge");
    frame(ui, 3);
    press("agent:send");
    const auto submissions_text = protocol.handle_agent("{\"id\":2,\"method\":\"bridge.poll\",\"bridge_token\":\"" + token + "\"}");
    relay::JsonParser parser(submissions_text);
    const auto submissions = parser.parse();
    const auto* result = relay::field(*submissions->object(), "result");
    const auto* entries = relay::field(*result->object(), "submissions")->array();
    check(entries->size() == 1 && *relay::field(*entries->front().object(), "message")->string() == "Hello bridge",
          "headless chat Send reaches only external bridge mailbox through protocol");
    publish(R"({"status":"Working","busy":true,"messages":[],"provider":{"selected":"compatible"}})");
    check(!ui.headless_item_rect("agent:send"), "busy composer replaces Send with Stop");
    press("agent:stop");
    const auto cancellation = protocol.handle_agent("{\"id\":3,\"method\":\"bridge.poll\",\"bridge_token\":\"" + token + "\"}");
    check(cancellation.find("\"cancel\":true") != std::string::npos, "headless Stop queues bridge cancellation");
    publish(R"({"status":"Ready","busy":false,"messages":[],"provider":{"selected":"compatible"}})");
    press("agent:message"); ImGui::GetIO().AddInputCharactersUTF8("First line"); frame(ui, 3);
    key(ui, ImGuiKey_Enter);
    check(consume().find("First line") == std::string::npos, "Ctrl+Enter inserts a newline without submitting");
    ImGui::GetIO().AddInputCharactersUTF8("Second line"); frame(ui, 3);
    key(ui, ImGuiKey_Enter, false);
    check(consume().find("First line\\nSecond line") != std::string::npos, "Enter sends the multiline composer text");
    press("agent:setup-tab"); frame(ui, 3);
    press("agent:endpoint"); ImGui::GetIO().AddInputCharactersUTF8("http://localhost:1234/v1/chat/completions"); frame(ui, 3);
    press("agent:model"); ImGui::GetIO().AddInputCharactersUTF8("fixture-model"); frame(ui, 3);
    press("agent:credential"); ImGui::GetIO().AddInputCharactersUTF8("fixture-ui-secret"); frame(ui, 3);
    press("agent:save-provider");
    (void)protocol.handle(R"({"id":12,"method":"chat.cancel"})");
    const auto settings = protocol.handle_agent("{\"id\":4,\"method\":\"bridge.poll\",\"bridge_token\":\"" + token + "\"}");
    check(settings.find("fixture-ui-secret") != std::string::npos && settings.find("fixture-model") != std::string::npos, "Stop preserves queued transient provider settings for authenticated bridge");
    const auto consumed = protocol.handle_agent("{\"id\":5,\"method\":\"bridge.poll\",\"bridge_token\":\"" + token + "\"}");
    check(consumed.find("fixture-ui-secret") == std::string::npos, "consumed credentials are absent from mailbox");
    check(protocol.handle_agent(R"({"id":6,"method":"session.audit"})").find("fixture-ui-secret") == std::string::npos, "provider credentials never enter audit");
    const std::string catalog = R"("models":[{"model":"fixture-a","displayName":"Fixture A","supportedReasoningEfforts":[{"reasoningEffort":"low","description":"Fast"},{"reasoningEffort":"high","description":"Careful"}]},{"model":"fixture-b","displayName":"Fixture B","supportedReasoningEfforts":[]}],"model":"fixture-a","effort":"low")";
    publish(R"({"status":"Sign in with ChatGPT","busy":false,"provider":{"selected":"openai"},"openai":{"account":null,"login":null,)" + catalog + "}}");
    press("agent:signin");
    check(consume().find("\"action\":\"signin\"") != std::string::npos, "Sign in button queues supported device authentication control");
    publish(R"({"status":"Finish sign-in","busy":false,"provider":{"selected":"openai"},"openai":{"account":null,"login":{"userCode":"FIXT-1234","verificationUrl":"https://auth.openai.com/codex/device"},)" + catalog + "}}");
    check(ui.headless_item_rect("agent:copy-device-code").has_value() && ui.headless_item_rect("agent:open-signin").has_value(), "device code controls rendered without browser interaction");
    press("agent:cancel-signin");
    check(consume().find("cancel_signin") != std::string::npos, "Cancel sign-in queues bridge control");
    press("agent:chat-tab"); frame(ui, 3);
    check(!ui.headless_item_rect("agent:model-picker") && !ui.headless_item_rect("agent:reasoning-slider"), "model and reasoning stay hidden until the common menu opens");
    press("agent:options"); frame(ui, 3);
    press("agent:model-picker"); frame(ui, 3);
    press("agent:model-option:fixture-b");
    check(consume().find("fixture-b") != std::string::npos, "model picker submits discovered model");
    auto slider = ui.headless_item_rect("agent:reasoning-slider");
    check(slider.has_value(), "common menu renders reasoning slider");
    (*slider)[0] = (*slider)[2] - 62; click(ui, *slider);
    check(consume().find("\"effort\":\"high\"") != std::string::npos, "reasoning picker submits supported effort");
    key(ui, ImGuiKey_Escape, false);
    key(ui, ImGuiKey_Escape, false);
    ImGui::SetWindowFocus("Agent"); frame(ui, 3);
    check(!ui.headless_item_rect("agent:new-chat") && !ui.headless_item_rect("agent:expand"), "chat header actions removed");
    auto* composer = static_cast<ImGuiWindow*>(nullptr);
    for (auto* window : ImGui::GetCurrentContext()->Windows) if (std::string(window->Name).starts_with("Agent/Composer_") && window->ParentWindow == ImGui::FindWindowByName("Agent")) composer = window;
    check(composer && composer->ScrollMax.y == 0 && composer->ScrollMax.x == 0, "composer contents fit without internal overflow");
    const auto send_rect = ui.headless_item_rect("agent:send");
    const auto options_rect = ui.headless_item_rect("agent:options");
    check(send_rect && options_rect && std::abs((*send_rect)[2] - composer->WorkRect.Max.x) < 2, "send icon aligned to composer right edge");
    check((*options_rect)[2] < (*send_rect)[0] && std::abs((*options_rect)[1] - (*send_rect)[1]) < 1, "compact model and send controls share a right-aligned row");
    check((*options_rect)[2] - (*options_rect)[0] < composer->WorkRect.GetWidth() - 50, "model selector sizes to its label instead of filling row");
    const auto usage_fixture = [&](int five_hour, int weekly) {
        publish("{\"status\":\"Ready\",\"busy\":false,\"provider\":{\"selected\":\"openai\"},\"openai\":{\"usage\":{\"fiveHour\":" + std::to_string(five_hour) + ",\"weekly\":" + std::to_string(weekly) + "}," + catalog + "}}");
    };
    const auto has_fill = [&](ImU32 color) {
        for (const auto& vertex : ImGui::FindWindowByName("Agent")->DrawList->VtxBuffer) if (vertex.col == color) return true;
        return false;
    };
    usage_fixture(80, 90);
    check(has_fill(IM_COL32(195, 201, 211, 255)) && has_fill(IM_COL32(246, 199, 65, 255)), "usage fill stays grey at 80 and yellow at 90");
    usage_fixture(81, 91);
    check(has_fill(IM_COL32(246, 199, 65, 255)) && has_fill(IM_COL32(242, 86, 86, 255)), "usage fill turns yellow above 80 and red above 90");
    const auto five_meter = ui.headless_item_rect("agent:usage:fiveHour"), weekly_meter = ui.headless_item_rect("agent:usage:weekly"), attach_button = ui.headless_item_rect("agent:attach"), model_menu = ui.headless_item_rect("agent:options");
    check(five_meter && weekly_meter && attach_button && model_menu && (*five_meter)[1] > (*attach_button)[3] && std::abs((*five_meter)[1] - (*weekly_meter)[1]) < .01F && (*five_meter)[2] < (*weekly_meter)[0] && std::abs(((*five_meter)[2] - (*five_meter)[0]) - ((*weekly_meter)[2] - (*weekly_meter)[0])) < .01F && std::abs((*five_meter)[0] - (composer->Pos.x + 4)) < 2 && std::abs((*weekly_meter)[2] - (composer->Pos.x + composer->Size.x - 4)) < 2 && (*five_meter)[1] >= composer->Pos.y + composer->Size.y, "both usage meters fill an equal-width row below composer controls");
    const std::string selectable_reply = "Selectable reply with café 日本語 and enough words to wrap across several visual lines while preserving copied text. More words for a longer paragraph.\nA second paragraph.";
    publish("{\"status\":\"Ready\",\"busy\":false,\"messages\":[{\"role\":\"assistant\",\"content\":\"" + relay::json_escape(selectable_reply) + "\"}],\"provider\":{\"selected\":\"compatible\"}}");
    press("agent:history-text"); key(ui, ImGuiKey_A); key(ui, ImGuiKey_C);
    check(std::string(ImGui::GetClipboardText()) == selectable_reply, "history selection copies original Unicode and real line breaks without soft wrapping");
    ImGui::GetIO().AddInputCharactersUTF8("cannot edit"); frame(ui, 3); key(ui, ImGuiKey_C);
    check(std::string(ImGui::GetClipboardText()) == selectable_reply, "selected history text remains read-only");
    std::string long_messages;
    for (int i = 0; i < 24; ++i) { if (i) long_messages += ","; long_messages += R"({"role":"assistant","content":"A long reply with multiple lines.\nMore details to fill the transcript.\nAnother line of details."})"; }
    publish(R"({"status":"Ready","busy":false,"provider":{"selected":"openai"},"messages":[)" + long_messages + "],\"openai\":{" + catalog + "}}");
    auto* conversation = ImGui::FindWindowByName("Agent/Conversation");
    if (!conversation) for (auto* window : ImGui::GetCurrentContext()->Windows) if (std::string(window->Name).starts_with("Agent/Conversation_") && window->ParentWindow == ImGui::FindWindowByName("Agent")) conversation = window;
    check(conversation && conversation->ScrollMax.y > 100, "long conversation overflows transcript");
    check(conversation->Scroll.y >= conversation->ScrollMax.y - 2, "new replies follow when already at bottom");
    ImGui::SetScrollY(conversation, 0); frame(ui, 4);
    check(ui.headless_item_rect("agent:jump-bottom").has_value(), "scrolling away from bottom reveals jump arrow");
    long_messages += R"(,{"role":"assistant","content":"A new streamed reply while reading earlier messages."})";
    publish(R"({"status":"Ready","busy":false,"provider":{"selected":"openai"},"messages":[)" + long_messages + "],\"openai\":{" + catalog + "}}");
    check(conversation->Scroll.y < 2 && ui.headless_item_rect("agent:jump-bottom"), "new replies do not pull a reader away from earlier messages");
    auto jump = ui.headless_item_rect("agent:jump-bottom"); (*jump)[0] = ((*jump)[0] + (*jump)[2]) / 2 - 60; click(ui, *jump); frame(ui, 5);
    check(conversation->Scroll.y >= conversation->ScrollMax.y - 2 && !ui.headless_item_rect("agent:jump-bottom"), "jump arrow returns to bottom and restores follow");
    ImGui::SetScrollY(conversation, 0); frame(ui, 3);
    ImGui::SetScrollY(conversation, conversation->ScrollMax.y); frame(ui, 3);
    check(!ui.headless_item_rect("agent:jump-bottom"), "manually scrolling to bottom restores follow");
    long_messages += R"(,{"role":"assistant","content":"A final reply after follow resumes."})";
    publish(R"({"status":"Ready","busy":false,"provider":{"selected":"openai"},"messages":[)" + long_messages + "],\"openai\":{" + catalog + "}}");
    check(conversation->Scroll.y >= conversation->ScrollMax.y - 2, "resumed follow tracks subsequent replies");
    key(ui, ImGuiKey_A, true, true); frame(ui, 3);
    check(ImGui::FindWindowByName("Agent")->DockId == 0 && ImGui::FindWindowByName("Agent")->Size.y > 600, "agent shortcut opens a full in-editor workspace without native windows");
    ImGui::SetWindowSize("Agent", ImVec2(320, 780)); frame(ui, 5);
    const auto narrow_send = ui.headless_item_rect("agent:send");
    const auto narrow_options = ui.headless_item_rect("agent:options");
    check(composer->ScrollMax.y == 0 && composer->ScrollMax.x == 0 && narrow_send && narrow_options, "narrow composer fits without internal scrolling");
    const auto narrow_five = ui.headless_item_rect("agent:usage:fiveHour"), narrow_weekly = ui.headless_item_rect("agent:usage:weekly");
    check(narrow_five && narrow_weekly && (*narrow_five)[2] < (*narrow_weekly)[0] && std::abs((*narrow_five)[1] - (*narrow_weekly)[1]) < .01F && std::abs((*narrow_five)[3] - (*narrow_five)[1] - 18) < .01F,
          "narrow usage pills stay side by side at slimmer readable height");
    check((*narrow_options)[0] >= composer->WorkRect.Min.x && (*narrow_send)[2] <= composer->WorkRect.Max.x + 1, "narrow composer controls remain within bounding box");
    press("agent:message");
    const std::string long_input = "A long input that should wrap at word boundaries without changing its submitted text. UTF-8: café 日本語. More words to make multiple display lines.";
    ImGui::GetIO().AddInputCharactersUTF8(long_input.c_str()); frame(ui, 4);
    auto* input_state = ImGui::GetInputTextState(ImGui::GetActiveID());
    check(input_state && std::string(input_state->TextA.Data).find('\n') != std::string::npos, "composer soft-wraps a long input while editing");
    key(ui, ImGuiKey_Enter, false);
    check(consume().find(relay::json_escape(long_input)) != std::string::npos, "soft wrapping preserves submitted text and Unicode");
    press("agent:message"); ImGui::GetIO().AddInputCharactersUTF8(long_input.c_str()); frame(ui, 4);
    input_state = ImGui::GetInputTextState(ImGui::GetActiveID());
    const auto first_break = std::string(input_state->TextA.Data).find('\n');
    key(ui, ImGuiKey_Home); key(ui, ImGuiKey_End, false); key(ui, ImGuiKey_Delete, false);
    auto edited_input = long_input; edited_input.erase(first_break, 1);
    key(ui, ImGuiKey_Enter, false);
    check(consume().find(relay::json_escape(edited_input)) != std::string::npos, "Delete crosses a soft line boundary without getting stuck");
    relay::WrappedInput wrapped; wrapped.width = 80; wrapped.raw = "abc\n\n日本語 café and a long unbrokenwordwithoutspaces"; wrapped.wrap();
    for (int offset : wrapped.breaks)
        check(wrapped.display[static_cast<std::size_t>(offset + 1)] != ' ' && wrapped.display[static_cast<std::size_t>(offset + 1)] != '\t', "wrapped words do not start with separating blanks");
    for (int position = 0; position <= static_cast<int>(wrapped.raw.size()); ++position)
        check(wrapped.raw_position(wrapped.display_position(position)) == position, "wrapped cursor positions round-trip");
    const auto original_text = wrapped.raw; wrapped.edit(wrapped.display);
    check(wrapped.raw == original_text, "soft wrapping preserves hard blank lines and Unicode");
    (void)protocol.handle(R"({"id":89,"method":"session.auto_approval","enabled":false})");
    const auto camera_revision = engine.scene_history().revision();
    protocol.set_editor_camera_handler([&](std::string_view request) { return ui.handle_camera_request(request); });
    check(protocol.handle_agent(R"({"id":90,"method":"editor.camera.set","distance":12})").find("capability denied") != std::string::npos, "camera controls require agent approval");
    (void)protocol.handle(R"({"id":91,"method":"session.auto_approval","enabled":true})");
    check(protocol.handle_agent(R"({"id":92,"method":"editor.camera.set","target_x":2,"target_y":3,"target_z":4,"yaw":0,"pitch":0,"distance":12})").find("\"ok\":true") != std::string::npos, "approved camera control reaches editor view");
    check(ui.view_override() && ui.view_override()->position.z == 16 && ui.view_override()->target.x == 2, "camera tool updates captured viewpoint immediately");
    const auto camera_entity = engine.scene().create("Camera frame fixture");
    const auto frame_request = "{\"id\":93,\"method\":\"editor.camera.frame\",\"entity\":\"" + camera_entity.to_string() + "\"}";
    check(protocol.handle_agent(frame_request).find("\"ok\":true") != std::string::npos, "camera frame uses native entity bounds");
    check(ui.selected_entities().empty(), "agent framing does not change human selection");
    check(protocol.handle_agent(R"({"id":94,"method":"editor.camera.set","pitch":2})").find("\"ok\":false") != std::string::npos, "camera tool rejects invalid angles");
    check(protocol.handle_agent(R"({"id":95,"method":"editor.camera.set","mode":"scene"})").find("\"ok\":true") != std::string::npos && !ui.view_override(), "camera tool can restore scene camera rendering");
    check(engine.scene_history().revision() == camera_revision, "inspection camera controls stay out of scene revision and undo");
    check(engine.scene_history().revision() == revision, "chat does not modify scene revision or undo");
    std::cout << "Headless chat send/stop tests passed without provider calls\n";
}

void media_ui(const std::filesystem::path& fixture) {
    std::filesystem::create_directories("captures");
    std::filesystem::copy_file(fixture / "image.png", "captures/chat-image.png");
    std::filesystem::copy_file(fixture / "clip.webm", "captures/chat-video.webm");
    relay::EditorUi ui([](std::string_view) { return std::string("{}"); });
    std::string error; check(ui.initialize_headless(error), "media headless context initializes");
    relay::ChatMedia media;
    ImRect last_media_item;
    auto draw = [&](std::string_view message) {
        auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2(800, 600);
        ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(700, 590)); ImGui::Begin("Media fixture");
        const bool handled = media.draw(message, true);
        last_media_item = ImGui::GetCurrentContext()->LastItemData.Rect;
        ImGui::End(); media.draw_viewer(true); ImGui::Render(); return handled;
    };
    check(draw("![unsafe](/etc/passwd)"), "out-of-scope media is rejected as unavailable");
    check(ImGui::GetCurrentContext()->UserTextures.empty(), "unsafe media does not load a texture");
    for (int i = 0; i < 500 && ImGui::GetCurrentContext()->UserTextures.Size == 0; ++i) {
        draw("![image](captures/chat-image.png)"); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(ImGui::GetCurrentContext()->UserTextures.Size == 1, "PNG is decoded asynchronously into an inline image texture");
    auto& viewer_io = ImGui::GetIO();
    const auto image_message = "![image](captures/chat-image.png)";
    viewer_io.AddMousePosEvent(100, 100); draw(image_message);
    viewer_io.AddMouseButtonEvent(0, true); draw(image_message);
    viewer_io.AddMouseButtonEvent(0, false); draw(image_message); draw(image_message);
    check(media.viewer_open(), "thumbnail click opens large media viewer");
    auto image_bounds = [&]() {
        auto* window = ImGui::FindWindowByName("Chat media viewer");
        check(window && window->Active, "large viewer is an active modal");
        for (const auto& command : window->DrawList->CmdBuffer) {
            if (command.TexRef._TexData != ImGui::GetCurrentContext()->UserTextures.front() || command.ElemCount < 6) continue;
            const auto& vertices = window->DrawList->VtxBuffer;
            const auto& indices = window->DrawList->IdxBuffer;
            const auto minimum = vertices[static_cast<int>(command.VtxOffset + indices[static_cast<int>(command.IdxOffset)])].pos;
            const auto maximum = vertices[static_cast<int>(command.VtxOffset + indices[static_cast<int>(command.IdxOffset + 2)])].pos;
            return ImRect(minimum, maximum);
        }
        throw std::runtime_error("large viewer has no image draw command");
    };
    const auto fitted = image_bounds();
    viewer_io.AddMousePosEvent(400, 268); draw(image_message);
    viewer_io.AddMouseWheelEvent(0, 1); draw(image_message);
    check(image_bounds().GetWidth() > fitted.GetWidth(), "scroll wheel zooms the large picture");
    const auto zoomed = image_bounds();
    viewer_io.AddMouseButtonEvent(0, true); draw(image_message);
    viewer_io.AddMousePosEvent(430, 288); draw(image_message); draw(image_message);
    viewer_io.AddMouseButtonEvent(0, false); draw(image_message);
    check(image_bounds().Min.x > zoomed.Min.x + 20, "dragging pans the large picture");
    viewer_io.AddKeyEvent(ImGuiKey_Escape, true); draw(image_message);
    check(!media.viewer_open(), "Escape closes the large viewer");
    viewer_io.AddKeyEvent(ImGuiKey_Escape, false); draw(image_message); draw(image_message);
    viewer_io.AddMousePosEvent(100, 100); draw(image_message);
    viewer_io.AddMouseButtonEvent(0, true); draw(image_message);
    viewer_io.AddMouseButtonEvent(0, false); draw(image_message); draw(image_message);
    viewer_io.AddMousePosEvent(5, 5); draw(image_message);
    viewer_io.AddMouseButtonEvent(0, true); draw(image_message);
    check(!media.viewer_open(), "clicking the dark backdrop closes the large viewer");
    viewer_io.AddMouseButtonEvent(0, false); draw(image_message);

#ifdef RELAY_CHAT_VIDEO
    for (int i = 0; i < 500 && ImGui::GetCurrentContext()->UserTextures.Size < 2; ++i) {
        draw("![video](captures/chat-video.webm)"); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(ImGui::GetCurrentContext()->UserTextures.Size == 2, "WebM poster is decoded into inline video texture");
    draw("![video](captures/chat-video.webm)");
    const auto slider_rect = last_media_item;
    auto* poster = ImGui::GetCurrentContext()->UserTextures.back();
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(slider_rect.Max.x - 15, (slider_rect.Min.y + slider_rect.Max.y) * .5F);
    draw("![video](captures/chat-video.webm)");
    io.AddMouseButtonEvent(0, true); draw("![video](captures/chat-video.webm)");
    io.AddMouseButtonEvent(0, false); draw("![video](captures/chat-video.webm)");
    for (int i = 0; i < 500 && ImGui::GetCurrentContext()->UserTextures.back() == poster; ++i) {
        draw("![video](captures/chat-video.webm)"); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(ImGui::GetCurrentContext()->UserTextures.back() != poster, "headless video seeking decodes a new frame");
    viewer_io.AddMousePosEvent(100, 100); draw("![video](captures/chat-video.webm)");
    viewer_io.AddMouseButtonEvent(0, true); draw("![video](captures/chat-video.webm)");
    viewer_io.AddMouseButtonEvent(0, false); draw("![video](captures/chat-video.webm)"); draw("![video](captures/chat-video.webm)");
    check(media.viewer_open(), "video thumbnail opens large media mode");
    auto* video_frame = ImGui::GetCurrentContext()->UserTextures.back();
    viewer_io.AddMousePosEvent(40, 554); draw("");
    viewer_io.AddMouseButtonEvent(0, true); draw("");
    viewer_io.AddMouseButtonEvent(0, false); draw("");
    for (int i = 0; i < 500 && ImGui::GetCurrentContext()->UserTextures.back() == video_frame; ++i) {
        draw(""); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(ImGui::GetCurrentContext()->UserTextures.back() != video_frame, "large video plays even when the chat thumbnail is not drawn");
    viewer_io.AddKeyEvent(ImGuiKey_Escape, true); draw("");
    check(!media.viewer_open(), "Escape also closes large video mode");
    viewer_io.AddKeyEvent(ImGuiKey_Escape, false); draw("");

#endif
    media.clear(); check(ImGui::GetCurrentContext()->UserTextures.empty(), "media textures unregister on cleanup");
}

} // namespace

void components_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    const auto node = engine.scene().create("Plain");
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize component editor without windows");
    frame(ui, 5);
    click(ui, *ui.headless_item_rect("entity:" + node.to_string()));
    frame(ui, 3);
    check(ui.headless_item_rect("inspector:add_component").has_value(),
          "Inspector ends with an Add Component button");
    for (const char* id : {"camera", "mesh_renderer", "light", "collider", "physics_body", "keyframes"})
        check(!ui.headless_item_rect(std::string{"inspector:component:"} + id),
              "Inspector shows no section for a component the node lacks");
    click_center(ui, *ui.headless_item_rect("inspector:add_component"));
    frame(ui, 2);
    const auto light = ui.headless_item_rect("component_window:item:light");
    check(light.has_value() && ui.headless_item_rect("component_window:item:camera") &&
              ui.headless_item_rect("component_window:category:Physics"),
          "the Add Component window lists categories and engine components");
    click_center(ui, *ui.headless_item_rect("component_window:category:Physics"));
    frame(ui, 2);
    check(!ui.headless_item_rect("component_window:item:light") &&
              ui.headless_item_rect("component_window:item:collider"),
          "choosing a category narrows the list");
    click_center(ui, *ui.headless_item_rect("component_window:category:All"));
    click_center(ui, *ui.headless_item_rect("component_window:item:light"));
    click_center(ui, *ui.headless_item_rect("component_window:add"));
    frame(ui, 3);
    check(engine.scene().get(node)->light.has_value(), "choosing a component and Add adds it");
    check(ui.headless_item_rect("inspector:component:light").has_value(),
          "an added component gets its Inspector section");
    click_center(ui, *ui.headless_item_rect("inspector:add_component"));
    frame(ui, 2);
    // A present component is drawn disabled, so neither clicking it nor Add does anything.
    click_center(ui, *ui.headless_item_rect("component_window:item:light"));
    click_center(ui, *ui.headless_item_rect("component_window:add"));
    key(ui, ImGuiKey_Escape, false);
    check(engine.scene().get(node)->light.has_value() && !ui.headless_item_rect("component_window:add"),
          "a present component cannot be added twice, and Escape closes the window");
    check(protocol.handle(R"({"id":3,"method":"component.remove","entity":")" + node.to_string() +
                          R"(","component":"light"})").find("\"ok\":true") != std::string::npos,
          "remove the light through the protocol");
    frame(ui, 40);
    check(!ui.headless_item_rect("inspector:component:light"),
          "a removed component's section disappears entirely");

    // The Hierarchy's Add Node window creates typed nodes from the inheritance tree.
    const auto before = engine.scene().entities().size();
    click_center(ui, *ui.headless_item_rect("hierarchy:add_node"));
    frame(ui, 2);
    check(ui.headless_item_rect("node_window:type:Node") && ui.headless_item_rect("node_window:type:RigidBody") &&
              ui.headless_item_rect("node_window:type:PointLight"),
          "the Add Node window shows the node type tree");
    const auto parent = *ui.headless_item_rect("node_window:type:PhysicsBody");
    const auto child = *ui.headless_item_rect("node_window:type:RigidBody");
    check(child[0] > parent[0] && child[1] > parent[1], "subtypes are indented under their parent");
    click(ui, child);
    click_center(ui, *ui.headless_item_rect("node_window:create"));
    frame(ui, 3);
    check(engine.scene().entities().size() == before + 1U, "Create adds one node");
    relay::Entity created{};
    for (const auto entity : engine.scene().entities())
        if (engine.scene().get(entity)->name == "Rigid Body") created = entity;
    check(created.valid() && engine.scene().get(created)->collider &&
              engine.scene().get(created)->physics_body &&
              engine.scene().get(created)->parent == node,
          "the created node has its type's inherited components, under the selected node");
    click_center(ui, *ui.headless_item_rect("hierarchy:add_node"));
    frame(ui, 2);
    click(ui, *ui.headless_item_rect("node_window:type:PhysicsBody"));
    click_center(ui, *ui.headless_item_rect("node_window:create"));
    key(ui, ImGuiKey_Escape, false);
    check(engine.scene().entities().size() == before + 1U, "category types cannot be created");
    std::cout << "Headless component Inspector tests passed\n";
}

// The Inspector's Joint section picks the connected body and the joint type.
void joints_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    auto& scene = engine.scene();
    const auto anchor = scene.create("Anchor");
    (void)scene.set_physics_body(anchor, relay::PhysicsBody{relay::PhysicsBody::Type::static_body});
    const auto swing = scene.create("Swing");
    (void)scene.set_physics_body(swing, relay::PhysicsBody{});
    (void)scene.set_joint(swing, relay::Joint{});
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize joint editor without windows");
    frame(ui, 5);
    click(ui, *ui.headless_item_rect("entity:" + swing.to_string()));
    frame(ui, 3);
    if (auto* inspector = ImGui::FindWindowByName("Inspector")) {
        ImGui::SetScrollY(inspector, inspector->ScrollMax.y);
        frame(ui, 3);
    }
    check(ui.headless_item_rect("inspector:component:joint") && ui.headless_item_rect("joint:type") &&
              ui.headless_item_rect("joint:connected"),
          "the Inspector shows the Joint section");
    click_center(ui, *ui.headless_item_rect("joint:connected"));
    frame(ui, 2);
    check(!ui.headless_item_rect("joint:connected:" + swing.to_string()).has_value(),
          "a node cannot be joined to itself");
    click_center(ui, *ui.headless_item_rect("joint:connected:" + anchor.to_string()));
    frame(ui, 3);
    check(scene.get(swing)->joint->connected == anchor, "choosing a body connects the joint to it");
    click_center(ui, *ui.headless_item_rect("joint:type"));
    frame(ui, 2);
    click_center(ui, *ui.headless_item_rect("joint:type:slider"));
    frame(ui, 3);
    check(scene.get(swing)->joint->type == relay::Joint::Type::slider &&
              scene.get(swing)->joint->limit_max == 1.0,
          "choosing a type changes the joint and its default limits");
    std::cout << "Headless joint Inspector tests passed\n";
}

// The Profiler panel reads recorded frames through the protocol, lists hotspots, pauses the
// profiler and inspects a single frame from the frame-time graph.
void profiler_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize profiler editor without windows");
    auto& profiler = relay::profiler();
    profiler.set_paused(false);
    profiler.clear();
    const auto slow = profiler.intern("Slow system");
    // Frames are recorded the way the live editor loop records them.
    const auto profiled_frames = [&](const int count) {
        for (int index = 0; index < count; ++index) {
            profiler.begin_frame();
            {
                const relay::ProfileScope scope(slow);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            frame(ui);
            profiler.end_frame(static_cast<std::uint64_t>(index), true);
        }
    };
    frame(ui, 3);
    ui.set_panel_visible("Profiler", true);
    profiled_frames(30);
    check(ui.headless_item_rect("profiler:graph").has_value() &&
              ui.headless_item_rect("profiler:hotspot:Slow system").has_value(),
          "the Profiler panel shows the frame graph and the hotspot");
    click_center(ui, *ui.headless_item_rect("profiler:pause"));
    frame(ui, 2);
    check(profiler.paused(), "Pause stops the profiler recording");
    const auto graph = *ui.headless_item_rect("profiler:graph");
    click(ui, {graph[2] - 62.0F, graph[1], graph[2], graph[3]});
    frame(ui, 2);
    check(ui.headless_item_rect("profiler:average").has_value(),
          "clicking the frame graph inspects a single frame");
    click_center(ui, *ui.headless_item_rect("profiler:pause"));
    frame(ui, 2);
    check(!profiler.paused(), "Resume records frames again");
    std::cout << "Headless profiler tests passed\n";
}

// Hierarchy search, type filters and collapse/expand all, and the same button in Assets.
void hierarchy_search_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    check(protocol.handle(R"({"id":1,"method":"project.create","filename":"projects/searching/project.relayproject","name":"Searching"})")
              .find("\"ok\":true") != std::string::npos, "create search project");
    std::filesystem::create_directories("projects/searching/art/props/small");
    auto& scene = engine.scene();
    const auto lights = scene.create("Lights");
    const auto sun = scene.create("Sun", lights);
    (void)scene.set_light(sun, relay::Light{relay::Light::Type::directional});
    const auto lamp = scene.create("Lamp", lights);
    (void)scene.set_light(lamp, relay::Light{});
    const auto level = scene.create("Level");
    const auto floor = scene.create("Floor", level);
    (void)scene.set_collider(floor, relay::BoxCollider{});
    (void)scene.set_physics_body(floor, relay::PhysicsBody{relay::PhysicsBody::Type::static_body});
    const auto crate = scene.create("Crate");
    (void)scene.set_collider(crate, relay::BoxCollider{});
    (void)scene.set_physics_body(crate, relay::PhysicsBody{});
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize search editor without windows");
    frame(ui, 5);
    const auto row = [&](relay::Entity entity) { return ui.headless_item_rect("entity:" + entity.to_string()); };
    const auto result = [&](relay::Entity entity) {
        return ui.headless_item_rect("hierarchy:result:" + entity.to_string());
    };
    check(row(sun) && row(floor) && ui.headless_item_rect("hierarchy:expand_all"),
          "the Hierarchy starts expanded with a collapse/expand-all button");
    const auto expand = *ui.headless_item_rect("hierarchy:expand_all");
    const auto filter = *ui.headless_item_rect("hierarchy:filter");
    check(expand[2] <= filter[0] && std::abs(expand[1] - filter[1]) < 1.0F,
          "the collapse/expand-all button sits left of the filter button");
    click_center(ui, expand);
    frame(ui, 2);
    check(row(lights) && !row(sun) && !row(floor), "collapse all folds every node");
    click_center(ui, expand);
    frame(ui, 2);
    check(row(sun) && row(floor), "expand all opens every node again");
    click_center(ui, expand);
    frame(ui, 2);

    click_center(ui, *ui.headless_item_rect("hierarchy:search"));
    type_text(ui, "lA");
    check(result(lamp) && !result(sun) && !result(floor) && !row(lights),
          "searching lists matching nodes flat, ignoring case");
    key(ui, ImGuiKey_Escape, false);
    check(row(lights) && !result(lamp), "Escape clears the search");

    click_center(ui, filter);
    frame(ui, 2);
    click_center(ui, *ui.headless_item_rect("hierarchy:filter:Light"));
    click(ui, *ui.headless_item_rect("hierarchy:empty"));
    check(result(sun) && result(lamp) && !result(crate) && !result(lights),
          "filtering by a category includes its subtypes");
    click_center(ui, filter);
    frame(ui, 2);
    click_center(ui, *ui.headless_item_rect("hierarchy:filter:Light"));
    click_center(ui, *ui.headless_item_rect("hierarchy:filter:StaticBody"));
    click(ui, *ui.headless_item_rect("hierarchy:empty"));
    check(result(floor) && !result(crate) && !result(sun), "filters narrow the list to node types");
    double_click(ui, *result(floor));
    frame(ui, 3);
    check(row(floor) && !result(floor) && ui.selected_entities() == std::vector<relay::Entity>{floor},
          "double-clicking a result shows it in the tree, opening its parents, and selects it");

    // Assets: the same button opens every folder, or closes them all.
    check(!ui.headless_item_rect("asset:project.relayproject"), "the project file is not listed");
    const auto folders = *ui.headless_item_rect("assets:expand_all");
    check(folders[2] <= ui.headless_item_rect("assets:filter")->at(0),
          "the Assets collapse/expand-all button sits left of its filter button");
    click_center(ui, folders);
    frame(ui, 2);
    check(ui.headless_item_rect("asset:art/props/small").has_value(),
          "expand all lists and opens nested asset folders");
    click_center(ui, folders);
    frame(ui, 2);
    check(ui.headless_item_rect("asset:art") && !ui.headless_item_rect("asset:art/props"),
          "collapse all closes every asset folder");
    std::cout << "Headless hierarchy search tests passed\n";
}

void key_event(relay::EditorUi& ui, SDL_Scancode scancode) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = scancode;
    (void)ui.handle_event(&event);
    event.type = SDL_EVENT_KEY_UP;
    (void)ui.handle_event(&event);
    frame(ui, 2);
}

void game_configuration_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    check(protocol.handle(R"({"id":1,"method":"project.create","filename":"projects/configured/project.relayproject","name":"Configured"})")
              .find("\"ok\":true") != std::string::npos, "create configuration project");
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize configuration editor without windows");
    frame(ui, 5);
    click_center(ui, *ui.headless_item_rect("menu:edit"));
    const auto item = ui.headless_item_rect("menu:edit:game_configuration");
    check(item.has_value(), "the Edit menu offers Game Configuration");
    click_center(ui, *item);
    frame(ui, 3);
    check(ui.headless_item_rect("config:window") && ui.headless_item_rect("config:page:input"),
          "Game Configuration opens on its Input page");
    const auto add = ui.headless_item_rect("config:input:action:jump:add");
    check(add.has_value() && ui.headless_item_rect("config:input:chip:Space"),
          "the Input page lists default actions with readable bindings");
    click_center(ui, *add);
    check(ui.headless_item_rect("config:input:capture").has_value(),
          "adding a binding waits for a control");
    key_event(ui, SDL_SCANCODE_Q);
    const auto jump = [&] {
        const auto& actions = engine.input().map().actions;
        return *std::find_if(actions.begin(), actions.end(),
                             [](const relay::InputAction& action) { return action.name == "jump"; });
    };
    check(jump().bindings.back() == "key:q" && !ui.headless_item_rect("config:input:capture"),
          "pressing a key binds it to the action by physical position");
    {
        std::string load_error;
        const auto saved = relay::load_project("projects/configured/project.relayproject", load_error);
        check(saved && saved->input && saved->input->actions.size() == engine.input().map().actions.size(),
              "a binding change saves the input map in the project file");
    }
    click_center(ui, *ui.headless_item_rect("config:input:action:jump:add"));
    key_event(ui, SDL_SCANCODE_ESCAPE);
    check(!ui.headless_item_rect("config:input:capture") && jump().bindings.size() == 3U,
          "Escape cancels a binding capture");

    const auto keys = *ui.headless_item_rect("config:input:axis:move_x:keys");
    const auto remove = *ui.headless_item_rect("config:input:axis:move_x:delete");
    check(keys[2] <= remove[0] || keys[1] >= remove[3],
          "binding buttons wrap inside their column instead of covering Delete");
    click_center(ui, *ui.headless_item_rect("config:input:axis:move_x:keys"));
    key_event(ui, SDL_SCANCODE_J);
    key_event(ui, SDL_SCANCODE_L);
    const auto& move_x = engine.input().map().axes.front();
    check(move_x.bindings.back().negative == "key:j" && move_x.bindings.back().positive == "key:l",
          "a key pair is captured as negative, then positive");

    click_center(ui, *ui.headless_item_rect("config:input:new_action"));
    type_text(ui, "dash");
    key(ui, ImGuiKey_Enter, false);
    check(ui.headless_item_rect("config:input:capture").has_value(),
          "a new action immediately waits for its first binding");
    key_event(ui, SDL_SCANCODE_K);
    const auto& actions = engine.input().map().actions;
    check(actions.back().name == "dash" && actions.back().bindings == std::vector<std::string>{"key:k"},
          "a new action is added and bound");
    click_center(ui, *ui.headless_item_rect("config:input:reset"));
    check(engine.input().map().actions.size() == relay::default_input_map().actions.size(),
          "Reset to defaults restores the engine's map");

    click_center(ui, *ui.headless_item_rect("config:page:graphics"));
    frame(ui, 2);
    const auto global_illumination = ui.headless_item_rect("config:graphics:global_illumination");
    check(global_illumination && ui.headless_item_rect("config:graphics:reflections"),
          "the Graphics page offers global illumination and ray traced reflections");
    click_center(ui, *global_illumination);
    frame(ui, 2);
    {
        std::string load_error;
        const auto saved = relay::load_project("projects/configured/project.relayproject", load_error);
        check(saved && saved->graphics && !saved->graphics->global_illumination &&
                  saved->graphics->reflections,
              "unticking Global illumination saves the setting in the project file");
    }
    click_center(ui, *ui.headless_item_rect("config:graphics:global_illumination"));
    frame(ui, 2);
    check(engine.graphics_settings().global_illumination,
          "ticking it again turns global illumination back on");
    click_center(ui, *ui.headless_item_rect("config:graphics:frame_rate_limit:60"));
    frame(ui, 2);
    check(engine.graphics_settings().frame_rate_limit == 60U,
          "a frame rate preset saves the limit");
    click_center(ui, *ui.headless_item_rect("config:graphics:frame_rate_limit"));
    key(ui, ImGuiKey_A);
    type_text(ui, "90");
    key(ui, ImGuiKey_Enter, false);
    frame(ui, 2);
    check(engine.graphics_settings().frame_rate_limit == 90U,
          "typing a frame rate limit saves it when the field is left");
    click_center(ui, *ui.headless_item_rect("config:graphics:frame_rate_limit:0"));
    frame(ui, 2);
    check(engine.graphics_settings().frame_rate_limit == 0U, "Unlimited clears the limit");
    click_center(ui, *ui.headless_item_rect("config:graphics:vsync"));
    frame(ui, 2);
    {
        std::string load_error;
        const auto saved = relay::load_project("projects/configured/project.relayproject", load_error);
        check(engine.graphics_settings().vsync && saved && saved->graphics && saved->graphics->vsync,
              "ticking Vsync saves it in the project file");
    }
    click_center(ui, *ui.headless_item_rect("config:graphics:vsync"));
    frame(ui, 2);
    check(!engine.graphics_settings().vsync, "unticking Vsync turns it off again");
    std::cout << "Headless Game Configuration tests passed\n";
}

void game_input_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    check(protocol.handle(R"({"id":1,"method":"project.create","filename":"projects/locked/project.relayproject","name":"Locked"})")
              .find("\"ok\":true") != std::string::npos, "create mouse lock project");
    auto map = engine.input().map();
    map.lock_mouse = true;
    check(protocol.handle(R"({"id":2,"method":"input.set_map","map":")" +
                          relay::json_escape(relay::input_map_json(map)) + "\"}")
              .find("\"ok\":true") != std::string::npos, "the project asks for mouse lock");
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize game input editor without windows");
    frame(ui, 5);
    SDL_Event run_key{};
    run_key.type = SDL_EVENT_KEY_DOWN;
    run_key.key.scancode = SDL_SCANCODE_F5;
    check(ui.handle_event(&run_key) && engine.status().mode == relay::RuntimeMode::game,
          "F5 runs the game from the editor");
    run_key.key.repeat = true;
    check(ui.handle_event(&run_key) && engine.status().mode == relay::RuntimeMode::game,
          "holding F5 does not restart the game");
    run_key.type = SDL_EVENT_KEY_UP;
    check(ui.handle_event(&run_key), "F5 release stays in the editor");
    frame(ui, 40);
    const auto viewport = ui.headless_item_rect("viewport");
    check(viewport.has_value(), "the viewport is visible");
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(((*viewport)[0] + (*viewport)[2]) / 2, ((*viewport)[1] + (*viewport)[3]) / 2);
    frame(ui, 2);
    check(!ui.game_has_input(), "the game has no input until the viewport is clicked");
    SDL_Event click{};
    click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    click.button.button = SDL_BUTTON_LEFT;
    check(!ui.handle_event(&click), "the focusing click also reaches the game");
    frame(ui, 10);
    check(ui.game_has_input() && ui.pointer_locked_for_game(),
          "clicking the viewport gives the game input and keeps the pointer locked across frames");
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    check(!ui.handle_event(&motion), "mouse motion goes to the game while it has input");
    SDL_Event escape{};
    escape.type = SDL_EVENT_KEY_DOWN;
    escape.key.scancode = SDL_SCANCODE_ESCAPE;
    check(ui.handle_event(&escape) && !ui.game_has_input() && !ui.pointer_locked_for_game(),
          "Escape returns input and the pointer to the editor");
    check(ui.handle_event(&motion), "without input focus the editor keeps mouse motion from the game");
    // Pointer movement reaches the editor again after Escape, as it would on a desktop.
    io.AddMousePosEvent(((*viewport)[0] + (*viewport)[2]) / 2 + 5, ((*viewport)[1] + (*viewport)[3]) / 2);
    frame(ui, 2);
    (void)ui.handle_event(&click);
    frame(ui, 2);
    check(ui.game_has_input(), "clicking again gives input back");
    SDL_Event stop_key{};
    stop_key.type = SDL_EVENT_KEY_DOWN;
    stop_key.key.scancode = SDL_SCANCODE_F8;
    check(ui.handle_event(&stop_key) && engine.status().mode == relay::RuntimeMode::editor,
          "F8 stops the game while it owns keyboard input");
    stop_key.type = SDL_EVENT_KEY_UP;
    check(ui.handle_event(&stop_key), "F8 release stays in the editor");
    frame(ui, 40);
    check(!ui.game_has_input() && !ui.pointer_locked_for_game(), "stopping the game releases input");
    std::cout << "Headless game input focus tests passed\n";
}

// Custom templates sit in the Add Node tree under the node type their root inherits, marked by a
// palette icon whose tooltip says "Custom template".
void node_templates_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    check(protocol.handle(R"({"id":1,"method":"project.create","filename":"projects/templated/project.relayproject","name":"Templated"})")
              .find("\"ok\":true") != std::string::npos, "create template project");
    const auto crate = engine.scene().create("Crate");
    (void)engine.scene().set_collider(crate, relay::BoxCollider{});
    (void)engine.scene().set_physics_body(crate, relay::PhysicsBody{});
    check(protocol.handle(R"({"id":2,"method":"templates.save","entity":")" + crate.to_string() +
                          R"(","name":"Crate"})").find("\"ok\":true") != std::string::npos,
          "save a rigid body template");
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize template editor without windows");
    frame(ui, 5);
    click_center(ui, *ui.headless_item_rect("hierarchy:add_node"));
    frame(ui, 2);
    const auto rigid = ui.headless_item_rect("node_window:type:RigidBody");
    const auto row = ui.headless_item_rect("node_window:template:Crate");
    const auto statics = ui.headless_item_rect("node_window:type:StaticBody");
    check(rigid && row && statics, "the Add Node tree shows the template with the node types");
    check((*row)[0] > (*rigid)[0] && (*row)[1] > (*rigid)[1] && (*row)[1] < (*statics)[1],
          "the template is nested under Rigid Body, the type its root inherits");
    const auto icon = ui.headless_item_rect("node_window:template_icon:Crate");
    check(icon && (*icon)[0] > (*row)[0] && (*icon)[1] >= (*row)[1] && (*icon)[3] <= (*row)[3],
          "a palette icon sits next to the template's name");
    check(!ui.headless_item_rect("tooltip:custom_template"), "no tooltip before hovering");
    ImGui::GetIO().AddMousePosEvent(((*icon)[0] + (*icon)[2]) / 2, ((*icon)[1] + (*icon)[3]) / 2);
    frame(ui, 3);
    check(ui.headless_item_rect("tooltip:custom_template").has_value(),
          "hovering the palette icon shows the Custom template tooltip");
    click(ui, *row);
    frame(ui, 2);
    check(ui.headless_item_rect("node_window:details_template_icon").has_value(),
          "the details pane marks the selection as a custom template");
    const auto before = engine.scene().entities().size();
    click_center(ui, *ui.headless_item_rect("node_window:create"));
    frame(ui, 3);
    check(engine.scene().entities().size() == before + 1U, "Create makes a copy of the template");
    std::cout << "Headless node template tests passed\n";
}

void scripts_ui() {
    relay::EngineConfig config;
    config.editor_mode = true;
    relay::Engine engine(config);
    relay::ControlProtocol protocol(engine);
    check(protocol.handle(R"({"id":1,"method":"project.create","filename":"projects/scripted/project.relayproject","name":"Scripted"})")
              .find("\"ok\":true") != std::string::npos, "create scripted project");
    std::filesystem::create_directories("projects/scripted/scripts");
    std::ofstream("projects/scripted/scripts/spin.cpp")
        << "#include \"relay_script.hpp\"\n"
           "class Spin : public relay::Behaviour {\n"
           "public:\n"
           "    void on_update(double dt) override {\n"
           "        self().set_rotation(self().rotation() + relay::Vec3{0, 90 * dt, 0});\n"
           "    }\n"
           "};\n"
           "RELAY_BEHAVIOUR(Spin)\n";
    const auto hero = engine.scene().create("Hero");
    check(protocol.handle(R"({"id":2,"method":"component.add","entity":")" + hero.to_string() +
                          R"(","component":"script","behaviour":"Spin"})").find("\"ok\":true") !=
              std::string::npos,
          "attach a script before the editor opens");
    relay::EditorUi ui([&](std::string_view request) { return protocol.handle(request); });
    std::string error;
    check(ui.initialize_headless(error), "initialize scripts editor without windows");
    frame(ui, 5);
    click(ui, *ui.headless_item_rect("entity:" + hero.to_string()));
    frame(ui, 3);
    check(ui.headless_item_rect("inspector:component:script:0").has_value(),
          "Inspector shows the script component of the selected entity");

    const auto run = ui.headless_item_rect("toolbar:run");
    check(run.has_value(), "toolbar exposes Run Game");
    click_center(ui, *run);
    frame(ui, 2);
    check(engine.status().mode == relay::RuntimeMode::editor,
          "Run Game does not start scripts in an untrusted project");
    const auto accept = ui.headless_item_rect("dialog:trust:accept");
    check(accept.has_value(), "Run Game asks the person to trust the project's scripts");
    click_center(ui, *accept);
    check(relay::project_scripts_trusted(engine.project()->root()),
          "accepting the prompt trusts the project");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{240};
    while (engine.status().mode != relay::RuntimeMode::game &&
           std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        frame(ui, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    check(engine.status().mode == relay::RuntimeMode::game,
          "Run Game builds the scripts and then starts the game");
    engine.step(30);
    check(std::abs(engine.scene().get(hero)->transform.rotation_degrees.y - 45.0) < 1e-6,
          "the built behaviour runs in the game the editor started");
    click_center(ui, *ui.headless_item_rect("toolbar:run"));
    check(engine.status().mode == relay::RuntimeMode::editor &&
              engine.scene().get(hero)->transform.rotation_degrees.y == 0.0,
          "Stop Game restores the scene the script changed");
    std::cout << "Headless script editor tests passed\n";
}

int main() {
    const auto original = std::filesystem::current_path();
    const auto temporary = std::filesystem::temp_directory_path() /
        ("relay-headless-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(temporary);
    std::filesystem::current_path(temporary);
    const auto trust_file = (temporary / "trusted-script-projects").string();
    setenv("RELAY_SCRIPT_TRUST_PATH", trust_file.c_str(), 1);
    int result = 0;
    // Each scenario starts from the default layout; persisted panels would otherwise carry over.
    const auto fresh = [](auto&& scenario) {
        std::filesystem::remove(".relay/headless-layout.ini");
        scenario();
    };
    try {
        fresh(run);
        fresh(project_ui);
        fresh(hierarchy_and_assets_ui);
        fresh(components_ui);
        fresh(node_templates_ui);
        fresh(joints_ui);
        fresh(hierarchy_search_ui);
        fresh(profiler_ui);
        fresh(game_configuration_ui);
        fresh(game_input_ui);
        fresh(scripts_ui);
        fresh(layout_persistence_ui);
        fresh(agent_ui);
        fresh(chat_ui);
        fresh([] { media_ui(RELAY_CHAT_MEDIA_FIXTURES); });
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    std::filesystem::current_path(original);
    std::filesystem::remove_all(temporary);
    return result;
}
