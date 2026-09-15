#include "relay/core/hash.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace relay {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

std::uint32_t big_endian_word(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

} // namespace

Sha256::Sha256()
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
             0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

void Sha256::compress(const std::uint8_t* block) {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16U; ++index) {
        schedule[index] = big_endian_word(block + index * 4U);
    }
    for (std::size_t index = 16U; index < 64U; ++index) {
        const auto previous = schedule[index - 15U];
        const auto ahead = schedule[index - 2U];
        const auto s0 = std::rotr(previous, 7) ^ std::rotr(previous, 18) ^ (previous >> 3U);
        const auto s1 = std::rotr(ahead, 17) ^ std::rotr(ahead, 19) ^ (ahead >> 10U);
        schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
    }
    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < 64U; ++index) {
        const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choose = (e & f) ^ (~e & g);
        const auto temp1 = h + s1 + choose + round_constants[index] + schedule[index];
        const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, const std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    total_bits_ += static_cast<std::uint64_t>(size) * 8U;
    std::size_t offset = 0;
    if (buffered_ != 0U) {
        const auto take = std::min(buffer_.size() - buffered_, size);
        std::memcpy(buffer_.data() + buffered_, bytes, take);
        buffered_ += take;
        offset += take;
        if (buffered_ < buffer_.size()) return;
        compress(buffer_.data());
        buffered_ = 0U;
    }
    for (; offset + buffer_.size() <= size; offset += buffer_.size()) {
        compress(bytes + offset);
    }
    if (offset < size) {
        buffered_ = size - offset;
        std::memcpy(buffer_.data(), bytes + offset, buffered_);
    }
}

std::string Sha256::hex() const {
    Sha256 copy = *this;
    const auto bit_length = copy.total_bits_;
    constexpr std::uint8_t padding_start = 0x80U;
    copy.update(&padding_start, 1U);
    constexpr std::uint8_t zero = 0U;
    while (copy.buffered_ != 56U) {
        copy.update(&zero, 1U);
    }
    std::array<std::uint8_t, 8> length_bytes{};
    for (std::size_t index = 0; index < 8U; ++index) {
        length_bytes[index] = static_cast<std::uint8_t>((bit_length >> (56U - index * 8U)) & 0xFFU);
    }
    // update() would add these to the running length; write them straight into the final block.
    std::memcpy(copy.buffer_.data() + 56U, length_bytes.data(), length_bytes.size());
    copy.compress(copy.buffer_.data());

    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(64U);
    for (const auto word : copy.state_) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            output += digits[(word >> static_cast<unsigned>(shift)) & 0xFU];
        }
    }
    return output;
}

} // namespace relay
