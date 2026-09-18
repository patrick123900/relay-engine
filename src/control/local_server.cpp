#include "relay/control/local_server.hpp"

#include "relay/control/control_protocol.hpp"
#include "relay/control/session_auth.hpp"
#include "relay/core/json.hpp"
#include "relay/core/engine.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace relay {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(const Socket socket) { closesocket(socket); }
std::string socket_error() { return "Windows socket error " + std::to_string(WSAGetLastError()); }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(const Socket socket) { close(socket); }
std::string socket_error() { return std::strerror(errno); }
#endif

bool send_all(const Socket socket, const std::string_view payload) {
    std::size_t sent = 0;
    while (sent < payload.size()) {
#ifdef _WIN32
        const auto count = send(socket, payload.data() + sent,
                                static_cast<int>(payload.size() - sent), 0);
#else
        const auto count = send(socket, payload.data() + sent, payload.size() - sent, 0);
#endif
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

} // namespace

bool serve_local_control(Engine& engine, const std::uint16_t port, std::string& error) {
    const auto* configured_token = std::getenv("RELAY_AGENT_TOKEN");
    const std::string token = configured_token ? configured_token : "";
    if (!session_token_matches(token, token)) {
        error = "loopback control requires a host-provisioned RELAY_AGENT_TOKEN (32–128 characters)";
        return false;
    }
#ifdef _WIN32
    WSADATA winsock_data{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        error = socket_error();
        return false;
    }
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == invalid_socket) {
        error = socket_error();
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    int reuse = 1;
#ifdef _WIN32
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        error = socket_error();
        close_socket(listener);
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    engine.pause();
    engine.logs().write(LogLevel::info,
                        "Local control listening on 127.0.0.1:" + std::to_string(port));
    while (engine.status().running) {
        const Socket client = accept(listener, nullptr, nullptr);
        if (client == invalid_socket) {
            error = socket_error();
            break;
        }
        ControlProtocol protocol(engine);
        protocol.configure_agent_from_environment();
        bool authenticated = false;
        const auto authentication_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
#ifdef _WIN32
        DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
        std::string pending;
        std::array<char, 4096> bytes{};
        while (engine.status().running) {
            if (!authenticated && std::chrono::steady_clock::now() >= authentication_deadline) break;
#ifdef _WIN32
            const auto received = recv(client, bytes.data(), static_cast<int>(bytes.size()), 0);
#else
            const auto received = recv(client, bytes.data(), bytes.size(), 0);
#endif
            if (received <= 0) break;
            pending.append(bytes.data(), static_cast<std::size_t>(received));
            if (pending.size() > (authenticated ? 1048576U : 2048U)) break;
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                auto request = pending.substr(0, newline);
                pending.erase(0, newline + 1U);
                if (!request.empty() && request.back() == '\r') request.pop_back();
                if (request.empty()) continue;
                if (!authenticated) {
                    JsonParser parser(request);
                    const auto value = parser.parse();
                    const auto* object = value ? value->object() : nullptr;
                    const auto* method = object ? field(*object, "method") : nullptr;
                    const auto* supplied = object ? field(*object, "token") : nullptr;
                    const auto* id = object ? field(*object, "id") : nullptr;
                    const bool authorized = object && object->size() == 3 && method && method->string() &&
                        *method->string() == "session.authenticate" && supplied && supplied->string() &&
                        id && id->number() && *id->number() >= 0 && std::floor(*id->number()) == *id->number() &&
                        session_token_matches(token, *supplied->string());
                    if (!authorized) {
                        engine.logs().write(LogLevel::warning, "Local control session authentication denied");
                        (void)send_all(client, "{\"id\":0,\"ok\":false,\"error\":\"session authentication required\"}\n");
                        pending.clear();
                        break;
                    }
                    const auto response = json_stringify(JsonValue{JsonValue::Object{
                        {"id", *id}, {"ok", JsonValue{true}},
                        {"result", JsonValue{JsonValue::Object{{"authenticated", JsonValue{true}}}}}}}) + '\n';
                    if (!send_all(client, response)) break;
                    authenticated = true;
#ifdef _WIN32
                    timeout = 60000;
                    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
                    timeout.tv_sec = 60;
                    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
                    continue;
                }
                auto response = protocol.handle_agent(request);
                response.push_back('\n');
                if (!send_all(client, response)) break;
            }
            if (!authenticated && pending.empty()) break;
        }
        close_socket(client);
    }
    close_socket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return error.empty();
}

} // namespace relay
