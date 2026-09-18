#pragma once
#include <string_view>
#include <cstddef>

namespace relay {
// The expected token is provisioned by the host, never returned or logged by the protocol.
inline bool session_token_matches(std::string_view expected, std::string_view supplied) {
    if (expected.size() < 32 || expected.size() > 128 || supplied.size() != expected.size()) return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < expected.size(); ++i)
        difference |= static_cast<unsigned char>(expected[i]) ^ static_cast<unsigned char>(supplied[i]);
    return difference == 0;
}
} // namespace relay
