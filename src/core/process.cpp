#include "relay/core/process.hpp"

#include <algorithm>
#include <array>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace relay {
namespace {

void append_bounded(std::string& destination, const char* bytes, const std::size_t size,
                    const std::size_t maximum_output) {
    const auto available = maximum_output > destination.size()
                               ? maximum_output - destination.size() : 0U;
    destination.append(bytes, std::min(size, available));
}

#ifdef _WIN32
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              output.data(), length);
    return output;
}

std::wstring quote_windows_argument(const std::string& argument) {
    const auto value = widen(argument);
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring output{L'"'};
    std::size_t slashes = 0U;
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'"') {
            output.append(slashes * 2U + 1U, L'\\');
            output.push_back(character);
            slashes = 0U;
        } else {
            output.append(slashes, L'\\');
            output.push_back(character);
            slashes = 0U;
        }
    }
    output.append(slashes * 2U, L'\\');
    output.push_back(L'"');
    return output;
}

} // namespace

ProcessResult run_process(const std::vector<std::string>& arguments,
                          const std::chrono::seconds timeout, const std::size_t maximum_output) {
    ProcessResult result;
    if (arguments.empty()) return result;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (CreatePipe(&read_pipe, &write_pipe, &security, 0) == 0) return result;
    (void)SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command.push_back(L' ');
        command += quote_windows_argument(argument);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    result.started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
                                    &startup, &process) != 0;
    CloseHandle(write_pipe);
    if (!result.started) {
        CloseHandle(read_pipe);
        return result;
    }
    const auto job = CreateJobObjectW(nullptr, nullptr);
    bool job_assigned = false;
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        (void)SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                      sizeof(limits));
        job_assigned = AssignProcessToJobObject(job, process.hProcess) != 0;
    }
    (void)ResumeThread(process.hThread);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> buffer{};
    while (true) {
        DWORD available = 0U;
        if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) != 0 && available > 0U) {
            DWORD read = 0U;
            if (ReadFile(read_pipe, buffer.data(),
                         std::min<DWORD>(available, static_cast<DWORD>(buffer.size())),
                         &read, nullptr) != 0) {
                append_bounded(result.output, buffer.data(), read, maximum_output);
            }
        }
        if (WaitForSingleObject(process.hProcess, 10U) == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            if (job_assigned) (void)TerminateJobObject(job, 124U);
            else (void)TerminateProcess(process.hProcess, 124U);
            (void)WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
    }
    DWORD available = 0U;
    DWORD read = 0U;
    while (PeekNamedPipe(read_pipe, nullptr, 0U, nullptr, &available, nullptr) != 0 &&
           available > 0U &&
           ReadFile(read_pipe, buffer.data(),
                    std::min<DWORD>(available, static_cast<DWORD>(buffer.size())),
                    &read, nullptr) != 0 && read > 0U) {
        append_bounded(result.output, buffer.data(), read, maximum_output);
    }
    DWORD exit_code = 1U;
    (void)GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job != nullptr) CloseHandle(job);
    CloseHandle(read_pipe);
    return result;
}
#else
} // namespace

ProcessResult run_process(const std::vector<std::string>& arguments,
                          const std::chrono::seconds timeout, const std::size_t maximum_output) {
    ProcessResult result;
    if (arguments.empty()) return result;
    std::vector<char*> values;
    values.reserve(arguments.size() + 1U);
    for (const auto& argument : arguments) {
        values.push_back(const_cast<char*>(argument.c_str()));
    }
    values.push_back(nullptr);
    std::array<int, 2> output_pipe{};
    if (pipe(output_pipe.data()) != 0) return result;
    const auto child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return result;
    }
    if (child == 0) {
        (void)setpgid(0, 0);
        (void)dup2(output_pipe[1], STDOUT_FILENO);
        (void)dup2(output_pipe[1], STDERR_FILENO);
        close(output_pipe[0]);
        close(output_pipe[1]);
        const auto null_input = open("/dev/null", O_RDONLY);
        if (null_input >= 0) {
            (void)dup2(null_input, STDIN_FILENO);
            close(null_input);
        }
        execvp(values.front(), values.data());
        _exit(127);
    }
    result.started = true;
    (void)setpgid(child, child);
    close(output_pipe[1]);
    const auto flags = fcntl(output_pipe[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> buffer{};
    int status = 0;
    while (true) {
        const auto count = read(output_pipe[0], buffer.data(), buffer.size());
        if (count > 0) append_bounded(result.output, buffer.data(), static_cast<std::size_t>(count), maximum_output);
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            (void)kill(-child, SIGKILL);
            (void)waitpid(child, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    while (true) {
        const auto count = read(output_pipe[0], buffer.data(), buffer.size());
        if (count <= 0) break;
        append_bounded(result.output, buffer.data(), static_cast<std::size_t>(count), maximum_output);
    }
    close(output_pipe[0]);
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    return result;
}
#endif

} // namespace relay
