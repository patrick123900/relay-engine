#include "relay/editor/ide.hpp"

#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace relay {

const std::vector<IdePreset>& ide_presets() {
    static const std::vector<IdePreset> presets{
        {"system", "System default", ""},
        {"vscode", "Visual Studio Code", "code {project} --goto {file}:{line}"},
        {"vscodium", "VSCodium", "codium {project} --goto {file}:{line}"},
        {"cursor", "Cursor", "cursor {project} --goto {file}:{line}"},
        {"zed", "Zed", "zed {project} {file}:{line}"},
        {"sublime", "Sublime Text", "subl {project} {file}:{line}"},
        {"kate", "Kate", "kate --line {line} {file}"},
        {"clion", "CLion", "clion {project} --line {line} {file}"},
        {"qtcreator", "Qt Creator", "qtcreator {file}:{line}"},
        {"custom", "Custom command", ""},
    };
    return presets;
}

const IdePreset* find_ide_preset(const std::string_view id) {
    for (const auto& preset : ide_presets())
        if (preset.id == id) return &preset;
    return nullptr;
}

std::vector<std::string> ide_command_line(const std::string_view command_template,
                                          const std::filesystem::path& file, const int line,
                                          const std::filesystem::path& project) {
    const auto substitute = [&](std::string word) {
        for (const auto& [name, value] : {std::pair<std::string, std::string>{"{file}", file.string()},
                                          {"{line}", std::to_string(line)},
                                          {"{project}", project.string()}}) {
            for (auto at = word.find(name); at != std::string::npos; at = word.find(name, at + value.size()))
                word.replace(at, name.size(), value);
        }
        return word;
    };
    std::vector<std::string> arguments;
    std::string word;
    bool in_word = false, quoted = false;
    for (const char character : command_template) {
        if (character == '"') {
            quoted = !quoted;
            in_word = true;
        } else if (character == ' ' && !quoted) {
            if (in_word) arguments.push_back(substitute(word));
            word.clear();
            in_word = false;
        } else {
            word += character;
            in_word = true;
        }
    }
    if (quoted) return {};
    if (in_word) arguments.push_back(substitute(word));
    return arguments;
}

bool ide_program_found(const std::string& program) {
    std::error_code error;
    if (program.find('/') != std::string::npos || program.find('\\') != std::string::npos)
        return std::filesystem::exists(program, error);
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;
#if defined(_WIN32)
    constexpr char separator = ';';
    const char* suffixes[] = {"", ".exe", ".cmd", ".bat"};
#else
    constexpr char separator = ':';
    const char* suffixes[] = {""};
#endif
    const std::string_view list(path);
    for (std::size_t start = 0; start <= list.size();) {
        auto end = list.find(separator, start);
        if (end == std::string_view::npos) end = list.size();
        const std::filesystem::path directory(list.substr(start, end - start));
        for (const auto* suffix : suffixes)
            if (!directory.empty() && std::filesystem::exists(directory / (program + suffix), error)) return true;
        start = end + 1U;
    }
    return false;
}

namespace {

#if defined(_WIN32)
// The command is run by cmd.exe so editors installed as .cmd shims start; characters cmd treats
// specially are refused rather than escaped.
bool safe_for_cmd(const std::string& argument) {
    return argument.find_first_of("&|<>^%\"") == std::string::npos;
}
#endif

} // namespace

std::string open_in_ide(const IdeLaunch& launch) {
    const auto* preset = find_ide_preset(launch.preset);
    if (preset == nullptr) return "unknown editor '" + launch.preset + "'";
    std::error_code error;
    const auto file = std::filesystem::absolute(launch.file, error).lexically_normal();
    const auto project = launch.project.empty() ? file.parent_path()
                                                : std::filesystem::absolute(launch.project, error).lexically_normal();
    std::vector<std::string> arguments;
    if (launch.preset == "system") {
#if defined(_WIN32)
        arguments = {"cmd.exe", "/c", "start", "", file.string()};
#elif defined(__APPLE__)
        arguments = {"open", file.string()};
#else
        arguments = {"xdg-open", file.string()};
#endif
    } else {
        const auto command = launch.preset == "custom" ? std::string_view(launch.custom_command) : preset->command;
        if (command.empty()) return "set a command for the custom editor in Editor preferences";
        arguments = ide_command_line(command, file, launch.line < 1 ? 1 : launch.line, project);
        if (arguments.empty()) return "the editor command is empty or has an unclosed quote";
    }
    if (!ide_program_found(arguments.front()))
        return "could not find '" + arguments.front() + "'; check Edit > Editor preferences";
#if defined(_WIN32)
    std::vector<std::string> command{"cmd.exe", "/c"};
    if (arguments.front() != "cmd.exe") {
        for (const auto& argument : arguments)
            if (!safe_for_cmd(argument)) return "the path contains characters this editor command cannot take";
        command.insert(command.end(), arguments.begin(), arguments.end());
    } else {
        command = arguments;
    }
    std::vector<const char*> pointers;
    for (const auto& argument : command) pointers.push_back(argument.c_str());
    pointers.push_back(nullptr);
    if (_spawnvp(_P_NOWAIT, pointers.front(), pointers.data()) == -1) return "could not start '" + arguments.front() + "'";
    return {};
#else
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    // The editor's own stdin and stdout can be the agent bridge's protocol pipe, so the child gets
    // /dev/null instead and its own session; otherwise a launcher's output or its reads from stdin
    // corrupt or end the conversation and the editor shuts down.
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
    pid_t child{};
    const int started = posix_spawnp(&child, pointers.front(), &actions, &attributes, pointers.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (started != 0) return "could not start '" + arguments.front() + "'";
    // Waited for on a detached thread so a finished editor launcher never lingers as a zombie.
    std::thread([child] {
        int status{};
        (void)waitpid(child, &status, 0);
    }).detach();
    return {};
#endif
}

} // namespace relay
