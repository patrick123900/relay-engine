#include "relay/scene/scene_history.hpp"

#include <algorithm>
#include <utility>

namespace relay {

SceneHistory::SceneHistory(Scene& scene, const std::size_t capacity)
    : scene_(scene), capacity_(std::max<std::size_t>(capacity, 1)) {
    undo_stack_.reserve(capacity_);
    redo_stack_.reserve(capacity_);
}

bool SceneHistory::execute(std::string label, const std::function<bool(Scene&)>& operation) {
    auto before = scene_.capture_state();
    if (!operation(scene_)) return false;
    if (undo_stack_.size() == capacity_) undo_stack_.erase(undo_stack_.begin());
    undo_stack_.push_back(Transaction{std::move(label), std::move(before), scene_.capture_state()});
    redo_stack_.clear();
    return true;
}

bool SceneHistory::undo() {
    if (undo_stack_.empty()) return false;
    auto transaction = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    scene_.restore_state(transaction.before);
    redo_stack_.push_back(std::move(transaction));
    return true;
}

bool SceneHistory::redo() {
    if (redo_stack_.empty()) return false;
    auto transaction = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    scene_.restore_state(transaction.after);
    undo_stack_.push_back(std::move(transaction));
    return true;
}

void SceneHistory::clear() {
    undo_stack_.clear();
    redo_stack_.clear();
}

std::size_t SceneHistory::undo_depth() const { return undo_stack_.size(); }
std::size_t SceneHistory::redo_depth() const { return redo_stack_.size(); }

std::string_view SceneHistory::next_undo_label() const {
    return undo_stack_.empty() ? std::string_view{} : undo_stack_.back().label;
}

std::string_view SceneHistory::next_redo_label() const {
    return redo_stack_.empty() ? std::string_view{} : redo_stack_.back().label;
}

} // namespace relay
