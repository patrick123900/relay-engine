#include "relay/audio/flac_encoder.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace relay {
namespace {

constexpr std::uint32_t block_size = 4096;
constexpr int maximum_order = 4;
constexpr int maximum_partition_order = 8;

// ---- Checksums ------------------------------------------------------------------------------

std::uint8_t crc8(const std::uint8_t* data, const std::size_t size) {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc = static_cast<std::uint8_t>(crc ^ data[i]);
        for (int bit = 0; bit < 8; ++bit) crc = static_cast<std::uint8_t>((crc & 0x80U) ? (crc << 1U) ^ 0x07U : crc << 1U);
    }
    return crc;
}

std::uint16_t crc16(const std::uint8_t* data, const std::size_t size) {
    static const auto table = [] {
        std::array<std::uint16_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i << 8U;
            for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x8000U) ? (crc << 1U) ^ 0x8005U : crc << 1U;
            t[i] = static_cast<std::uint16_t>(crc & 0xFFFFU);
        }
        return t;
    }();
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) crc = static_cast<std::uint16_t>((crc << 8U) ^ table[(crc >> 8U) ^ data[i]]);
    return crc;
}

// ---- Bits ------------------------------------------------------------------------------------

class BitWriter {
public:
    void write(const std::uint64_t value, const int bits) {
        if (bits <= 0) return;
        accumulator_ = (accumulator_ << bits) | (bits == 64 ? value : value & ((1ULL << bits) - 1ULL));
        count_ += bits;
        while (count_ >= 8) {
            bytes_.push_back(static_cast<std::uint8_t>((accumulator_ >> (count_ - 8)) & 0xFFU));
            count_ -= 8;
        }
    }
    // A signed value in `bits` bits of two's complement.
    void write_signed(const std::int64_t value, const int bits) { write(static_cast<std::uint64_t>(value), bits); }
    void rice(const std::uint32_t folded, const int parameter) {
        std::uint32_t quotient = folded >> parameter;
        while (quotient >= 32U) {
            write(0, 32);
            quotient -= 32U;
        }
        write(1, static_cast<int>(quotient) + 1); // `quotient` zeros, then a one.
        write(folded & ((1U << parameter) - 1U), parameter);
    }
    void pad() {
        if (count_ != 0) write(0, 8 - count_);
    }
    [[nodiscard]] std::vector<std::uint8_t>& bytes() { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t accumulator_{};
    int count_{};
};

std::uint32_t fold(const std::int64_t value) {
    return static_cast<std::uint32_t>((value << 1) ^ (value >> 63));
}

// ---- A subframe ------------------------------------------------------------------------------

struct Plan {
    enum class Kind { constant, verbatim, fixed } kind{Kind::verbatim};
    int order{};
    int partition_order{};
    std::vector<int> parameters;
    std::vector<std::int64_t> residual;
    std::uint64_t bits{};
};

std::vector<std::int64_t> fixed_residual(const std::vector<std::int32_t>& x, const int order) {
    std::vector<std::int64_t> residual;
    residual.reserve(x.size() - static_cast<std::size_t>(order));
    for (std::size_t i = static_cast<std::size_t>(order); i < x.size(); ++i) {
        const std::int64_t a = x[i];
        const auto at = [&](const std::size_t back) { return static_cast<std::int64_t>(x[i - back]); };
        switch (order) {
        case 0: residual.push_back(a); break;
        case 1: residual.push_back(a - at(1)); break;
        case 2: residual.push_back(a - 2 * at(1) + at(2)); break;
        case 3: residual.push_back(a - 3 * at(1) + 3 * at(2) - at(3)); break;
        default: residual.push_back(a - 4 * at(1) + 6 * at(2) - 4 * at(3) + at(4)); break;
        }
    }
    return residual;
}

std::uint64_t rice_bits(const std::vector<std::uint32_t>& folded, const std::size_t begin, const std::size_t end, const int parameter) {
    std::uint64_t bits = 0;
    for (std::size_t i = begin; i < end; ++i) bits += (folded[i] >> parameter) + 1U + static_cast<std::uint64_t>(parameter);
    return bits;
}

// Picks the partition order and Rice parameters that make the residual smallest.
void plan_residual(Plan& plan, const std::size_t samples) {
    std::vector<std::uint32_t> folded(plan.residual.size());
    for (std::size_t i = 0; i < folded.size(); ++i) folded[i] = fold(plan.residual[i]);
    std::uint64_t best_bits = ~0ULL;
    for (int order = 0; order <= maximum_partition_order; ++order) {
        const std::size_t parts = 1ULL << order;
        if (samples % parts != 0U || samples / parts <= static_cast<std::size_t>(plan.order)) break;
        std::uint64_t total = 4;
        std::vector<int> parameters;
        std::size_t begin = 0;
        for (std::size_t part = 0; part < parts; ++part) {
            const std::size_t length = samples / parts - (part == 0 ? static_cast<std::size_t>(plan.order) : 0U);
            const std::size_t end = begin + length;
            std::uint64_t best_part = ~0ULL;
            int best_parameter = 0;
            for (int k = 0; k <= 14; ++k) {
                const auto bits = rice_bits(folded, begin, end, k);
                if (bits < best_part) { best_part = bits; best_parameter = k; }
            }
            parameters.push_back(best_parameter);
            total += 4U + best_part;
            begin = end;
        }
        if (total < best_bits) {
            best_bits = total;
            plan.partition_order = order;
            plan.parameters = std::move(parameters);
        }
    }
    plan.bits = best_bits;
}

Plan plan_subframe(const std::vector<std::int32_t>& x, const int bps) {
    Plan best;
    best.kind = Plan::Kind::verbatim;
    best.bits = 8ULL + static_cast<std::uint64_t>(bps) * x.size();
    if (std::all_of(x.begin(), x.end(), [&](const std::int32_t v) { return v == x.front(); })) {
        best.kind = Plan::Kind::constant;
        best.bits = 8ULL + static_cast<std::uint64_t>(bps);
        return best;
    }
    for (int order = 0; order <= maximum_order; ++order) {
        if (x.size() <= static_cast<std::size_t>(order) + 1U) break;
        Plan candidate;
        candidate.kind = Plan::Kind::fixed;
        candidate.order = order;
        candidate.residual = fixed_residual(x, order);
        plan_residual(candidate, x.size());
        if (candidate.bits == ~0ULL) continue;
        candidate.bits += 8ULL + static_cast<std::uint64_t>(bps) * static_cast<std::uint64_t>(order) + 2U;
        if (candidate.bits < best.bits) best = std::move(candidate);
    }
    return best;
}

void write_subframe(BitWriter& out, const std::vector<std::int32_t>& x, const int bps, const Plan& plan) {
    out.write(0, 1); // Padding bit.
    switch (plan.kind) {
    case Plan::Kind::constant:
        out.write(0b000000, 6);
        out.write(0, 1); // No wasted bits.
        out.write_signed(x.front(), bps);
        return;
    case Plan::Kind::verbatim:
        out.write(0b000001, 6);
        out.write(0, 1);
        for (const auto v : x) out.write_signed(v, bps);
        return;
    case Plan::Kind::fixed: {
        out.write(0b001000U | static_cast<unsigned>(plan.order), 6);
        out.write(0, 1);
        for (int i = 0; i < plan.order; ++i) out.write_signed(x[static_cast<std::size_t>(i)], bps);
        out.write(0, 2); // Rice coding with 4 bit parameters.
        out.write(static_cast<unsigned>(plan.partition_order), 4);
        const std::size_t parts = 1ULL << plan.partition_order;
        std::size_t at = 0;
        for (std::size_t part = 0; part < parts; ++part) {
            const std::size_t length = x.size() / parts - (part == 0 ? static_cast<std::size_t>(plan.order) : 0U);
            const int parameter = plan.parameters[part];
            out.write(static_cast<unsigned>(parameter), 4);
            for (std::size_t i = 0; i < length; ++i) out.rice(fold(plan.residual[at + i]), parameter);
            at += length;
        }
        return;
    }
    }
}

// ---- WAV -------------------------------------------------------------------------------------

struct Pcm {
    std::uint32_t rate{};
    int channels{};
    int bits{};
    std::vector<std::vector<std::int32_t>> samples; // Per channel.
};

std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8U) | (p[2] << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U); }
std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8U)); }

bool parse_wav(const std::span<const std::uint8_t> wav, Pcm& pcm, std::string& error) {
    if (wav.size() < 44U || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    std::size_t at = 12;
    bool have_format = false;
    const std::uint8_t* data = nullptr;
    std::size_t data_size = 0;
    int format_tag = 0;
    int block_align = 0;
    while (at + 8U <= wav.size()) {
        const auto size = static_cast<std::size_t>(le32(wav.data() + at + 4));
        const auto* body = wav.data() + at + 8U;
        const auto available = std::min(size, wav.size() - at - 8U);
        if (std::memcmp(wav.data() + at, "fmt ", 4) == 0 && available >= 16U) {
            format_tag = le16(body);
            pcm.channels = le16(body + 2);
            pcm.rate = le32(body + 4);
            block_align = le16(body + 12);
            pcm.bits = le16(body + 14);
            // WAVE_FORMAT_EXTENSIBLE carries the real format in its sub-format.
            if (format_tag == 0xFFFE && available >= 26U) format_tag = le16(body + 24);
            have_format = true;
        } else if (std::memcmp(wav.data() + at, "data", 4) == 0) {
            data = body;
            data_size = available;
            break;
        }
        at += 8U + size + (size & 1U);
    }
    if (!have_format || data == nullptr) {
        error = "the WAV file has no format or no samples";
        return false;
    }
    if (format_tag != 1 || (pcm.bits != 8 && pcm.bits != 16 && pcm.bits != 24) || pcm.channels < 1 || pcm.channels > 8 ||
        pcm.rate == 0U || pcm.rate > 655350U || block_align != pcm.channels * (pcm.bits / 8)) {
        error = "only 8, 16 and 24 bit integer PCM WAV files are compressed";
        return false;
    }
    const auto width = static_cast<std::size_t>(pcm.bits / 8);
    const auto frames = data_size / (width * static_cast<std::size_t>(pcm.channels));
    if (frames == 0U) {
        error = "the WAV file holds no samples";
        return false;
    }
    pcm.samples.assign(static_cast<std::size_t>(pcm.channels), std::vector<std::int32_t>(frames));
    for (std::size_t frame = 0; frame < frames; ++frame)
        for (int channel = 0; channel < pcm.channels; ++channel) {
            const auto* p = data + (frame * static_cast<std::size_t>(pcm.channels) + static_cast<std::size_t>(channel)) * width;
            std::int32_t value;
            if (pcm.bits == 8) value = static_cast<std::int32_t>(p[0]) - 128; // 8 bit WAV is unsigned.
            else if (pcm.bits == 16) value = static_cast<std::int16_t>(le16(p));
            else value = (static_cast<std::int32_t>(p[0] | (p[1] << 8U) | (p[2] << 16U)) << 8) >> 8;
            pcm.samples[static_cast<std::size_t>(channel)][frame] = value;
        }
    return true;
}

// ---- Frames ----------------------------------------------------------------------------------

void write_utf8_number(std::vector<std::uint8_t>& out, const std::uint64_t value) {
    if (value < 0x80U) { out.push_back(static_cast<std::uint8_t>(value)); return; }
    int extra = 1;
    while (extra < 6 && value >= (1ULL << (6 * extra + 5 - extra + 1))) ++extra;
    // The classic UTF-8 layout: a lead byte with `extra` continuation bytes after it.
    const auto lead_mask = static_cast<std::uint8_t>(0xFFU << (7 - extra));
    out.push_back(static_cast<std::uint8_t>(lead_mask | (value >> (6 * extra))));
    for (int i = extra - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(0x80U | ((value >> (6 * i)) & 0x3FU)));
}

std::string encode_pcm(const Pcm& pcm) {
    const auto total = pcm.samples.front().size();
    std::string stream = "fLaC";
    // STREAMINFO, the last (and only) metadata block.
    BitWriter info;
    info.write(0x80, 8);
    info.write(34, 24);
    // The block size every frame but the last has (a stream's minimum may not be below 16, so a
    // short sound still says 4096 and ends in a short frame, as the reference encoder does).
    info.write(block_size, 16);
    info.write(block_size, 16);
    info.write(0, 24);
    info.write(0, 24);
    info.write(pcm.rate, 20);
    info.write(static_cast<unsigned>(pcm.channels - 1), 3);
    info.write(static_cast<unsigned>(pcm.bits - 1), 5);
    info.write(total, 36);
    for (int i = 0; i < 16; ++i) info.write(0, 8); // MD5 unknown.
    stream.append(info.bytes().begin(), info.bytes().end());

    std::uint64_t frame_number = 0;
    for (std::size_t start = 0; start < total; start += block_size, ++frame_number) {
        const auto length = std::min<std::size_t>(block_size, total - start);
        std::vector<std::vector<std::int32_t>> channels(pcm.samples.size());
        for (std::size_t c = 0; c < channels.size(); ++c)
            channels[c].assign(pcm.samples[c].begin() + static_cast<std::ptrdiff_t>(start),
                               pcm.samples[c].begin() + static_cast<std::ptrdiff_t>(start + length));

        // Stereo: the cheapest of left/right, left/side, side/right and mid/side.
        std::vector<std::vector<std::int32_t>> coded = channels;
        std::array<int, 8> widths{};
        widths.fill(pcm.bits);
        unsigned assignment = static_cast<unsigned>(pcm.channels - 1);
        if (pcm.channels == 2) {
            std::vector<std::int32_t> side(length), mid(length);
            for (std::size_t i = 0; i < length; ++i) {
                side[i] = channels[0][i] - channels[1][i];
                mid[i] = static_cast<std::int32_t>((static_cast<std::int64_t>(channels[0][i]) + channels[1][i]) >> 1);
            }
            const auto cost = [&](const std::vector<std::int32_t>& x, const int bps) { return plan_subframe(x, bps).bits; };
            const auto left = cost(channels[0], pcm.bits), right = cost(channels[1], pcm.bits);
            const auto side_cost = cost(side, pcm.bits + 1), mid_cost = cost(mid, pcm.bits);
            const auto independent = left + right, left_side = left + side_cost, side_right = side_cost + right, mid_side = mid_cost + side_cost;
            const auto best = std::min({independent, left_side, side_right, mid_side});
            if (best == independent) {
            } else if (best == left_side) {
                assignment = 8;
                coded = {channels[0], side};
                widths[1] = pcm.bits + 1;
            } else if (best == side_right) {
                assignment = 9;
                coded = {side, channels[1]};
                widths[0] = pcm.bits + 1;
            } else {
                assignment = 10;
                coded = {mid, side};
                widths[1] = pcm.bits + 1;
            }
        }

        BitWriter frame;
        frame.write(0xFFF8, 16); // Sync code, fixed block size.
        const bool full = length == block_size;
        frame.write(full ? 0b1100U : 0b0111U, 4); // 4096, or "16 bit size follows".
        frame.write(0, 4);                        // Sample rate from STREAMINFO.
        frame.write(assignment, 4);
        frame.write(0, 3);                        // Sample size from STREAMINFO.
        frame.write(0, 1);
        write_utf8_number(frame.bytes(), frame_number);
        if (!full) {
            frame.bytes().push_back(static_cast<std::uint8_t>(((length - 1U) >> 8U) & 0xFFU));
            frame.bytes().push_back(static_cast<std::uint8_t>((length - 1U) & 0xFFU));
        }
        frame.bytes().push_back(crc8(frame.bytes().data(), frame.bytes().size()));
        for (std::size_t c = 0; c < coded.size(); ++c) {
            const auto plan = plan_subframe(coded[c], widths[c]);
            write_subframe(frame, coded[c], widths[c], plan);
        }
        frame.pad();
        const auto checksum = crc16(frame.bytes().data(), frame.bytes().size());
        frame.bytes().push_back(static_cast<std::uint8_t>(checksum >> 8U));
        frame.bytes().push_back(static_cast<std::uint8_t>(checksum & 0xFFU));
        stream.append(frame.bytes().begin(), frame.bytes().end());
    }
    return stream;
}

} // namespace

std::optional<std::string> encode_wav_as_flac(const std::span<const std::uint8_t> wav, std::string& error) {
    Pcm pcm;
    if (!parse_wav(wav, pcm, error)) return std::nullopt;
    error.clear();
    return encode_pcm(pcm);
}

} // namespace relay
