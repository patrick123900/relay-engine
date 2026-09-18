#pragma once
#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>

namespace relay {
// Soft breaks are display-only: submitted text retains original whitespace/newlines and UTF-8.
struct WrappedInput {
    std::string raw, display;
    std::vector<int> breaks;
    float width{};
    int raw_position(int position) const {
        return position - static_cast<int>(std::count_if(breaks.begin(), breaks.end(), [position](int value) { return value < position; }));
    }
    int display_position(int position) const {
        int result = position;
        for (int value : breaks) if (value <= result) ++result;
        return result;
    }
    void edit(const std::string& text) {
        std::size_t prefix = 0, suffix = 0;
        while (prefix < display.size() && prefix < text.size() && display[prefix] == text[prefix]) ++prefix;
        while (suffix < display.size() - prefix && suffix < text.size() - prefix && display[display.size() - 1 - suffix] == text[text.size() - 1 - suffix]) ++suffix;
        std::string value;
        std::vector<int> remaining;
        for (int offset : breaks) {
            if (offset < static_cast<int>(prefix)) remaining.push_back(offset);
            else if (offset >= static_cast<int>(display.size() - suffix)) remaining.push_back(offset + static_cast<int>(text.size()) - static_cast<int>(display.size()));
        }
        for (std::size_t i = 0; i < text.size(); ++i) if (!std::binary_search(remaining.begin(), remaining.end(), static_cast<int>(i))) value += text[i];
        std::size_t limit = std::min<std::size_t>(4000, value.size());
        while (limit < value.size() && limit > 0 && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80) --limit;
        raw = value.substr(0, limit);
        display = text; breaks = std::move(remaining);
    }
    void wrap() {
        display.clear(); breaks.clear();
        const char* cursor = raw.c_str(); const char* end = cursor + raw.size();
        while (cursor < end) {
            if (*cursor == '\n') { display += '\n'; ++cursor; continue; }
            const char* newline = std::find(cursor, end, '\n');
            const char* line_end = ImGui::GetFont()->CalcWordWrapPosition(ImGui::GetFontSize(), cursor, newline, std::max(width, 1.0F));
            if (line_end <= cursor) { line_end = cursor + 1; while (line_end < newline && (static_cast<unsigned char>(*line_end) & 0xc0) == 0x80) ++line_end; }
            display.append(cursor, line_end);
            cursor = line_end;
            if (cursor < newline) { breaks.push_back(static_cast<int>(display.size())); display += '\n'; }
            else if (cursor < end && *cursor == '\n') { display += '\n'; ++cursor; }
        }
    }
    static int callback(ImGuiInputTextCallbackData* data) {
        auto& state = *static_cast<WrappedInput*>(data->UserData);
        const auto previous_raw = state.raw;
        const auto previous_size = state.display.size();
        state.edit(std::string(data->Buf, static_cast<std::size_t>(data->BufTextLen)));
        const int maximum = static_cast<int>(state.raw.size());
        int cursor = std::clamp(state.raw_position(data->CursorPos), 0, maximum), start = std::clamp(state.raw_position(data->SelectionStart), 0, maximum), finish = std::clamp(state.raw_position(data->SelectionEnd), 0, maximum);
        // Backspace/Delete cross a soft boundary by deleting a real character, not the layout break.
        if (state.raw == previous_raw && previous_size == static_cast<std::size_t>(data->BufTextLen) + 1 && start == finish) {
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && cursor > 0) {
                int begin = cursor - 1; while (begin > 0 && (static_cast<unsigned char>(state.raw[static_cast<std::size_t>(begin)]) & 0xc0) == 0x80) --begin;
                state.raw.erase(static_cast<std::size_t>(begin), static_cast<std::size_t>(cursor - begin)); cursor = start = finish = begin;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && cursor < static_cast<int>(state.raw.size())) {
                int end = cursor + 1; while (end < static_cast<int>(state.raw.size()) && (static_cast<unsigned char>(state.raw[static_cast<std::size_t>(end)]) & 0xc0) == 0x80) ++end;
                state.raw.erase(static_cast<std::size_t>(cursor), static_cast<std::size_t>(end - cursor));
            }
        }
        state.wrap();
        if (state.display != data->Buf) {
            data->DeleteChars(0, data->BufTextLen); data->InsertChars(0, state.display.c_str());
            data->CursorPos = state.display_position(cursor); data->SelectionStart = state.display_position(start); data->SelectionEnd = state.display_position(finish);
        }
        return 0;
    }
};
}
