#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace relay {

// View-only layout preferences; stored separately from scene/manifest files. The ImGui ini holds
// the dock arrangement plus a [Relay][Preferences] section for bound panel and view flags.
class EditorLayout {
  public:
    // Without an override or RELAY_EDITOR_LAYOUT_PATH, the layout lives in the user's config
    // directory, so it survives restarts, rebuilds and different working directories.
    void initialize(std::string override_path = {});
    // Persists a flag under `key`. Bind before the first frame, when ImGui reads the ini.
    void bind(std::string key, bool* value);
    void build(float scale);
    void save() const;
    void reset() { reset_pending_ = true; }
    [[nodiscard]] const std::string& path() const { return ini_path_; }

    void read_preference(std::string_view line);
    void write_preferences(std::string& output) const;
    bool preferences_loaded{false};

  private:
    struct Preference {
        std::string key;
        bool* value{};
        bool saved{};
    };
    std::string ini_path_;
    std::vector<Preference> preferences_;
    bool reset_pending_{false};
    bool controls_migrated_{false};
    bool optional_migrated_{false};
};

[[nodiscard]] std::string default_editor_layout_path();

} // namespace relay
