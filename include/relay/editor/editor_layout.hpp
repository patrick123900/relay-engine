#pragma once

#include <string>

namespace relay {

// View-only layout preferences; stored separately from scene/manifest files.
class EditorLayout {
  public:
    void initialize();
    void build(float scale);
    void save() const;
    void reset() { reset_pending_ = true; }

  private:
    std::string ini_path_;
    bool reset_pending_{false};
    bool controls_migrated_{false};
};

} // namespace relay
