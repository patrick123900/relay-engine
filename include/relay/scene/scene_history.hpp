#pragma once

#include "relay/scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

class SceneHistory {
public:
    explicit SceneHistory(Scene& scene, std::size_t capacity = 128);

    // A nonzero `gesture` folds this change into the previous transaction when that transaction
    // carries the same token, so a continuous drag remains a single undoable step. The token must
    // identify one gesture: matching on the label alone would wrongly merge a new drag into an
    // earlier, unrelated edit of the same entity.
    [[nodiscard]] bool execute(std::string label, const std::function<bool(Scene&)>& operation,
                               std::uint64_t gesture = 0);
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    void clear();

    [[nodiscard]] std::size_t undo_depth() const;
    [[nodiscard]] std::size_t redo_depth() const;
    // Names the authored content rather than counting changes: undoing back to a state that was
    // already saved restores that state's revision, so the editor stops reporting unsaved work
    // instead of treating the round trip as two further modifications. Runtime state that never
    // enters the history — animation playback time, for instance — deliberately leaves it alone.
    [[nodiscard]] std::uint64_t revision() const;
    [[nodiscard]] std::string_view next_undo_label() const;
    [[nodiscard]] std::string_view next_redo_label() const;
    // Labels newest first, so the editor can show the history as a list rather than a depth count.
    [[nodiscard]] std::vector<std::string> undo_labels() const;
    [[nodiscard]] std::vector<std::string> redo_labels() const;

private:
    struct Transaction {
        std::string label;
        SceneState before;
        SceneState after;
        std::uint64_t gesture{};
        // Unique per transaction, so the state it produced can be recognised again after an undo.
        std::uint64_t serial{};
    };

    Scene& scene_;
    std::size_t capacity_;
    std::vector<Transaction> undo_stack_;
    std::vector<Transaction> redo_stack_;
    std::uint64_t serial_counter_{0};
    // The revision of the state underneath the undo stack. It only moves when that state stops
    // being the one the stack was built on: a clear, or an eviction dropping the oldest entry.
    std::uint64_t base_revision_{0};
};

} // namespace relay
