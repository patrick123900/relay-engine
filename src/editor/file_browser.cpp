#include "relay/editor/file_browser.hpp"

#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace relay {
namespace {

#if !defined(_WIN32)
// Runs a program directly, without a shell, and reports whether it exited successfully.
bool run(const std::vector<std::string>& command) {
    std::vector<char*> arguments;
    for (const auto& argument : command) arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);
    pid_t child{};
    if (posix_spawnp(&child, arguments.front(), nullptr, nullptr, arguments.data(), environ) != 0)
        return false;
    int status{};
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string file_uri(const std::filesystem::path& path) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string uri = "file://";
    for (const unsigned char c : path.string()) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            uri += static_cast<char>(c);
        } else {
            uri += '%';
            uri += hex[c >> 4U];
            uri += hex[c & 15U];
        }
    }
    return uri;
}
#endif

} // namespace

void show_in_file_browser(const std::filesystem::path& path, const bool directory) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    if (error) return;
#if defined(_WIN32)
    const auto target = absolute.wstring();
    if (directory)
        (void)_wspawnlp(_P_NOWAIT, L"explorer.exe", L"explorer.exe", target.c_str(), nullptr);
    else
        (void)_wspawnlp(_P_NOWAIT, L"explorer.exe", L"explorer.exe",
                        (L"/select,\"" + target + L"\"").c_str(), nullptr);
#else
    // The helper process is waited for on a detached thread so it never lingers as a zombie.
    std::thread([absolute, directory] {
#if defined(__APPLE__)
        if (directory) run({"open", absolute.string()});
        else run({"open", "-R", absolute.string()});
#else
        // FileManager1 is the freedesktop interface Nautilus, Dolphin, Nemo and others implement
        // to select an item; without it, open the containing folder.
        const auto uri = "['" + file_uri(absolute) + "']";
        if (!run({"gdbus", "call", "--session", "--dest", "org.freedesktop.FileManager1",
                  "--object-path", "/org/freedesktop/FileManager1", "--method",
                  directory ? "org.freedesktop.FileManager1.ShowFolders"
                            : "org.freedesktop.FileManager1.ShowItems",
                  uri, ""}))
            run({"xdg-open", (directory ? absolute : absolute.parent_path()).string()});
#endif
    }).detach();
#endif
}

} // namespace relay
