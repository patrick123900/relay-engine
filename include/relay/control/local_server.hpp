#pragma once

#include <cstdint>
#include <string>

namespace relay {

class Engine;

// Serves Relay's newline-delimited control protocol on IPv4 loopback only. The call blocks until
// runtime.quit is received or an unrecoverable socket error occurs.
[[nodiscard]] bool serve_local_control(Engine& engine, std::uint16_t port, std::string& error);

} // namespace relay

