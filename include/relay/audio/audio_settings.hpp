#pragma once

#include "relay/core/json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// One effect in a bus's chain. Every parameter is stored, but each type reads only its own:
//   reverb      room_size, damping, width, pre_delay_ms, mix
//   delay       time_ms, feedback, damping, mix
//   eq          low_db (shelf at 200 Hz), mid_db at mid_frequency, high_db (shelf at 5 kHz)
//   compressor  threshold_db, ratio, attack_ms, release_ms, makeup_db
//   limiter     ceiling_db, release_ms
//   lowpass, highpass  cutoff_hz, resonance
struct AudioEffect {
    enum class Type : std::uint8_t { reverb, delay, eq, compressor, limiter, lowpass, highpass };
    Type type{Type::reverb};
    bool enabled{true};
    double mix{0.3};
    double room_size{0.6};
    double damping{0.5};
    double width{1.0};
    double pre_delay_ms{10.0};
    double time_ms{350.0};
    double feedback{0.35};
    double low_db{0.0};
    double mid_db{0.0};
    double mid_frequency{1000.0};
    double high_db{0.0};
    double threshold_db{-18.0};
    double ratio{4.0};
    double attack_ms{10.0};
    double release_ms{150.0};
    double makeup_db{0.0};
    double ceiling_db{-1.0};
    double cutoff_hz{8000.0};
    double resonance{0.707};

    friend bool operator==(const AudioEffect&, const AudioEffect&) = default;
};

inline constexpr std::size_t maximum_bus_effects = 8U;
[[nodiscard]] std::string_view audio_effect_type_name(AudioEffect::Type type);
[[nodiscard]] std::optional<AudioEffect::Type> audio_effect_type_from_name(std::string_view name);
// The parameters `type` uses, by their JSON and protocol names.
[[nodiscard]] std::vector<std::string_view> audio_effect_parameters(AudioEffect::Type type);
// A pointer to the named parameter, or null when it is not a parameter name.
[[nodiscard]] double* audio_effect_parameter(AudioEffect& effect, std::string_view name);
// Checks each parameter the effect's type uses against its range.
[[nodiscard]] bool valid_audio_effect(const AudioEffect& effect, std::string& error);

// One mixer bus. Sources play into a bus; every bus but Master feeds its parent, so a bus tree
// like Master > SFX > Footsteps turns a whole group of sounds up or down together.
struct AudioBus {
    std::string name;
    std::string parent; // Empty only for Master.
    double volume_db{0.0};
    bool mute{};
    // While any bus is soloed, only soloed buses, their ancestors and their descendants are heard.
    bool solo{};
    // Applied in order to everything reaching this bus, before its volume.
    std::vector<AudioEffect> effects;

    friend bool operator==(const AudioBus&, const AudioBus&) = default;
};

// Per-project mixer layout, saved in the project file under settings.audio. Master always comes
// first; parents come before their children.
struct AudioSettings {
    std::vector<AudioBus> buses;
    // How positioned sounds reach the ears: stereo panning for speakers, or binaural (a head
    // model with per-ear delay and shadow) for headphones.
    enum class Spatialization : std::uint8_t { stereo, binaural } spatialization{Spatialization::stereo};

    friend bool operator==(const AudioSettings&, const AudioSettings&) = default;
};

inline constexpr std::string_view master_audio_bus = "Master";
inline constexpr std::size_t maximum_audio_buses = 64U;
// Volumes at or below the floor are silent.
inline constexpr double minimum_audio_volume_db = -80.0;
inline constexpr double maximum_audio_volume_db = 24.0;

// Master, with a limiter, and Music, SFX, Ambience and Voice beneath it.
[[nodiscard]] AudioSettings default_audio_settings();
// Bus names follow input names: letters, digits, spaces, '_' and '-', at most 64 characters.
[[nodiscard]] bool valid_audio_bus_name(std::string_view name);
// Checks names, parents, cycles and ranges, and puts parents before children.
[[nodiscard]] bool normalize_audio_settings(AudioSettings& settings, std::string& error);
[[nodiscard]] std::string audio_settings_json(const AudioSettings& settings);
[[nodiscard]] std::optional<AudioSettings> parse_audio_settings(const JsonValue& value,
                                                                std::string& error);
[[nodiscard]] double audio_db_to_gain(double decibels);
[[nodiscard]] double audio_gain_to_db(double gain);

} // namespace relay
