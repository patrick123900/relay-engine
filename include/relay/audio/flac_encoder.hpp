#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace relay {

// Lossless compression of uncompressed WAV sounds, for cooked games: the samples come back exactly,
// in about half the bytes. Fixed predictors with Rice coding and stereo decorrelation: simpler and
// a little larger than the reference encoder, and plain FLAC to any decoder.
//
// Takes a RIFF/WAVE file of 8, 16 or 24 bit integer PCM (any rate, up to eight channels). Returns
// the FLAC stream, or nothing with `error` set for anything else (float samples, 32 bit, ADPCM,
// damaged files), which a build then leaves as it is.
[[nodiscard]] std::optional<std::string> encode_wav_as_flac(std::span<const std::uint8_t> wav, std::string& error);

} // namespace relay
