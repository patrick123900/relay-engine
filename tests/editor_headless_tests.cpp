#include "relay/control/control_protocol.hpp"
#include "relay/core/engine.hpp"
#include "relay/editor/editor_ui.hpp"
#include "relay/editor/chat_media.hpp"
#include "relay/editor/wrapped_input.hpp"
#include "relay/render/scene_render.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
    const auto row = [&](relay::Entity entity) {
        const auto rect = ui.headless_item_rect("entity:" + entity.to_string());
        check(rect.has_value(), "visible hierarchy row is recorded");
        return *rect;
    };
    click(ui, row(first));
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

int main() {
    const auto original = std::filesystem::current_path();
    const auto temporary = std::filesystem::temp_directory_path() /
        ("relay-headless-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(temporary);
    std::filesystem::current_path(temporary);
    int result = 0;
    try { run(); project_ui(); agent_ui(); chat_ui(); media_ui(RELAY_CHAT_MEDIA_FIXTURES); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    std::filesystem::current_path(original);
    std::filesystem::remove_all(temporary);
    return result;
}
