#include "relay/ui/ui_system.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace relay {
namespace {

bool interactive(const UiComponents& ui) {
    return (ui.button && !ui.button->disabled) || (ui.toggle && !ui.toggle->disabled) ||
           (ui.slider && !ui.slider->disabled);
}

// Where a view point falls along a slider's track, as its value.
double slider_value(const UiLayoutNode& node, const UiSlider& slider, Vec2 pointer) {
    const auto inverse = node.transform.inverse();
    if (!inverse) return slider.value;
    const auto local = inverse->apply(pointer);
    const double half = slider.handle_size * 0.5;
    double amount = 0.0;
    if (slider.vertical) {
        const double start = node.size.y - half, end = half;
        amount = end != start ? (local.y - start) / (end - start) : 0.0;
    } else {
        const double start = half, end = node.size.x - half;
        amount = end != start ? (local.x - start) / (end - start) : 0.0;
    }
    double value = slider.minimum + std::clamp(amount, 0.0, 1.0) * (slider.maximum - slider.minimum);
    if (slider.step > 0.0)
        value = std::min(slider.minimum + std::round((value - slider.minimum) / slider.step) * slider.step,
                         slider.maximum);
    return value;
}

constexpr std::array<std::string_view, 3> mouse_buttons{"mouse:left", "mouse:right", "mouse:middle"};

} // namespace

std::string_view ui_event_name(const UiEvent::Type type) {
    switch (type) {
    case UiEvent::Type::clicked: return "clicked";
    case UiEvent::Type::toggled: return "toggled";
    case UiEvent::Type::value_changed: return "value_changed";
    case UiEvent::Type::pressed: return "pressed";
    case UiEvent::Type::released: return "released";
    }
    return "clicked";
}

void UiSystem::set_view_size(const std::uint32_t width, const std::uint32_t height) {
    width_ = width;
    height_ = height;
}

void UiSystem::reset() {
    events_.clear();
    hovered_ = pressed_ = {};
    simulated_.reset();
}

void UiSystem::update(Scene& scene, InputState& input) {
    events_.clear();
    if (width_ == 0U || height_ == 0U) return;
    const auto layout = painter_.layout(ui_sources(scene), width_, height_);
    if (layout.nodes.empty()) {
        reset();
        return;
    }
    using Query = InputState::Query;
    bool pointer_on = !input.mouse_locked();
    Vec2 pointer{input.mouse_x(), input.mouse_y()};
    // A tap within one step presses and releases in that step, which is a whole click.
    bool went_down = input.raw_control("mouse:left", Query::pressed);
    bool went_up = input.raw_control("mouse:left", Query::released);
    if (simulated_) {
        const auto* node = layout.find(simulated_->control);
        if (!node || !node->visible) {
            simulated_.reset();
        } else {
            pointer = node->transform.apply({node->size.x * 0.5, node->size.y * 0.5});
            pointer_on = true;
            went_down = !simulated_->released;
            went_up = simulated_->released;
            if (simulated_->released) simulated_.reset();
            else simulated_->released = true;
        }
    }
    pointer_ = pointer;
    // The topmost control the pointer reaches. Interactive controls take it; a control that stops
    // the pointer ends the search; pass-through controls let the search continue below them.
    const UiLayoutNode* over = nullptr;
    bool blocked = false;
    if (pointer_on)
        for (auto it = layout.order.rbegin(); it != layout.order.rend(); ++it) {
            const auto& node = layout.nodes[*it];
            if (!node.visible || !node.ui->control ||
                node.ui->control->mouse_filter == UiControl::MouseFilter::ignore || !layout.contains(node, pointer))
                continue;
            if (interactive(*node.ui)) {
                over = &node;
                blocked = true;
                break;
            }
            if (node.ui->control->mouse_filter == UiControl::MouseFilter::stop) {
                blocked = true;
                break;
            }
        }
    hovered_ = over ? over->entity : Entity{};
    if (went_down && over) {
        pressed_ = over->entity;
        emit(UiEvent::Type::pressed, pressed_, 0.0);
    }
    if (went_down && blocked)
        for (const auto button : mouse_buttons)
            if (input.raw_control(button, Query::pressed)) input.consume(button);

    const auto* held = pressed_.valid() ? layout.find(pressed_) : nullptr;
    auto* record = pressed_.valid() ? scene.get(pressed_) : nullptr;
    if (pressed_.valid() && (!held || !record || !held->visible || !interactive(record->ui))) {
        pressed_ = {};
        held = nullptr;
        record = nullptr;
    }
    if (record && record->ui.slider && pointer_on) {
        auto& slider = *record->ui.slider;
        const double value = slider_value(*held, slider, pointer);
        if (value != slider.value) {
            slider.value = value;
            emit(UiEvent::Type::value_changed, pressed_, value);
        }
    }
    if (record && went_up) {
        auto& ui = record->ui;
        const auto control = pressed_;
        emit(UiEvent::Type::released, control, 0.0);
        if (over && over->entity == control) {
            std::string sound;
            if (ui.button) {
                auto& button = *ui.button;
                if (button.toggle) {
                    button.pressed = !button.pressed;
                    emit(UiEvent::Type::toggled, control, button.pressed ? 1.0 : 0.0);
                }
                emit(UiEvent::Type::clicked, control, button.pressed ? 1.0 : 0.0);
                sound = button.click_sound;
            } else if (ui.toggle) {
                auto& toggle = *ui.toggle;
                toggle.checked = !toggle.checked;
                emit(UiEvent::Type::toggled, control, toggle.checked ? 1.0 : 0.0);
                emit(UiEvent::Type::clicked, control, toggle.checked ? 1.0 : 0.0);
                sound = toggle.click_sound;
            } else if (ui.slider) {
                emit(UiEvent::Type::clicked, control, ui.slider->value);
            }
            if (!sound.empty() && sound_) sound_(sound);
        }
        pressed_ = {};
    }
    // A press the interface took stays hidden from the game until it is released.
    if (pressed_.valid()) input.consume("mouse:left");
}

UiLayout UiSystem::layout(const Scene& scene, const std::uint32_t width, const std::uint32_t height) {
    return painter_.layout(ui_sources(scene), width, height);
}

const UiDrawList& UiSystem::draw(const Scene& scene, const std::uint32_t width, const std::uint32_t height) {
    draw_list_ = painter_.draw(layout(scene, width, height), visual_state());
    return draw_list_;
}

bool UiSystem::simulate_click(const Scene& scene, const Entity control, std::string& error) {
    const auto current = layout(scene, width_, height_);
    const auto* node = current.find(control);
    if (!node || !node->ui->control) {
        error = "the entity is not a control";
        return false;
    }
    if (!node->visible) {
        error = "the control is hidden";
        return false;
    }
    if (!interactive(*node->ui)) {
        error = "the control is not an enabled button, check box or slider";
        return false;
    }
    simulated_ = Simulated{control, false};
    return true;
}

} // namespace relay
