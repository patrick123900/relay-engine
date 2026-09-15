#pragma once

#include "relay/scene/scene.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

class SceneHistory {
public:
    explicit SceneHistory(Scene& scene, std::size_t capacity = 128);

    [[nodiscard]] bool execute(std::string label, const std::function<bool(Scene&)>& operation);
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    void clear();

    [[nodiscard]] std::size_t undo_depth() const;
    [[nodiscard]] std::size_t redo_depth() const;
    [[nodiscard]] std::string_view next_undo_label() const;
    [[nodiscard]] std::string_view next_redo_label() const;

private:
    struct Transaction {
        std::string label;
        SceneState before;
        SceneState after;
    };

    Scene& scene_;
    std::size_t capacity_;
    std::vector<Transaction> undo_stack_;
    std::vector<Transaction> redo_stack_;
};

} // namespace relay
