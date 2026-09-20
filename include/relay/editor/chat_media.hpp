#pragma once
#include <memory>
#include <functional>
#include <string_view>
namespace relay {
// Local chat media, decoded on workers. Never opens a desktop window or network connection.
class ChatMedia {
public:
    ChatMedia();
    ~ChatMedia();
    bool draw(std::string_view markdown, bool headless, const std::function<void(std::string_view)>& text_renderer = {});
    void draw_viewer(bool headless);
    [[nodiscard]] bool viewer_open() const;
    void clear();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
