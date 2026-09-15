#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace relay {

// Dependency-free SHA-256. Asset identities are durable public data once a scene is saved, so they
// need a collision-resistant digest rather than a fast non-cryptographic hash.
class Sha256 {
public:
    Sha256();

    void update(const void* data, std::size_t size);

    // Finalizes a copy of the current state, so a digest can be read without ending the stream.
    [[nodiscard]] std::string hex() const;

private:
    void compress(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_{};
    std::uint64_t total_bits_{};
};

} // namespace relay
