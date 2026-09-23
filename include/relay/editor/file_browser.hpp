#pragma once

#include <filesystem>

namespace relay {

// Shows a file or folder in the operating system's file manager without blocking the editor: a file
// is selected in its folder where the platform supports it, a folder is opened. A human-only editor
// action; it is deliberately not part of the agent-facing control protocol.
void show_in_file_browser(const std::filesystem::path& path, bool directory);

} // namespace relay
