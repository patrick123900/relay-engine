#pragma once

#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace relay {

// View state only. The final clicked item is active; range selections follow visible tree order.
struct EditorSelection {
    std::vector<std::string> handles;
    std::string primary_handle;
    std::string anchor;
    [[nodiscard]] bool contains(const std::string& handle) const {
        return std::find(handles.begin(), handles.end(), handle) != handles.end();
    }
    void assign(const std::string& handle) {
        handles.clear();
        if (!handle.empty()) handles.push_back(handle);
        primary_handle = anchor = handle;
    }
    void click(const std::string& handle, bool toggle, bool range,
               std::span<const std::string> visible = {}) {
        if (range && !anchor.empty()) {
            const auto first = std::find(visible.begin(), visible.end(), anchor);
            const auto last = std::find(visible.begin(), visible.end(), handle);
            if (first != visible.end() && last != visible.end()) {
                if (!toggle) handles.clear();
                for (auto i = std::min(first, last); i <= std::max(first, last); ++i)
                    if (!contains(*i)) handles.push_back(*i);
                primary_handle = handle;
                return;
            }
        }
        if (!toggle) { assign(handle); return; }
        if (const auto i = std::find(handles.begin(), handles.end(), handle); i != handles.end())
            handles.erase(i);
        else if (!handle.empty()) handles.push_back(handle);
        primary_handle = contains(handle) ? handle : handles.empty() ? "" : handles.back();
        anchor = handle;
    }
    template <class Exists> void prune(Exists exists) {
        std::erase_if(handles, [&](const auto& handle) { return !exists(handle); });
        if (!contains(primary_handle)) primary_handle = handles.empty() ? "" : handles.back();
        if (!exists(anchor)) anchor.clear();
    }
};

} // namespace relay
