#pragma once

#include "relay/audio/audio_clip.hpp"
#include "relay/audio/audio_effects.hpp"
#include "relay/audio/audio_settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

inline constexpr std::uint32_t audio_output_rate = 48000U;
inline constexpr std::size_t maximum_audio_voices = 256U;
// Reverb zones heard at once, each with its own reverb.
inline constexpr std::size_t maximum_reverb_slots = 8U;

// What one voice should sound like now. Gains move to new values over the next rendered block,
// so parameter changes never click.
struct AudioVoiceTarget {
    float left{1.0F};
    float right{1.0F};
    // Playback speed: 1 is the clip's own pitch, 2 an octave up.
    double rate{1.0};
    // Mix stereo clips to mono before the gains apply, as positioned sounds do.
    bool downmix{};
    bool loop{};
    bool paused{};
    std::string bus{std::string(master_audio_bus)};
    // How much reaches each zone reverb, as gains on top of the dry sound.
    std::array<float, maximum_reverb_slots> sends{};
    // A one-pole low-pass for muffled (occluded, or behind a binaural listener) sounds; at or
    // above 20 kHz it is off.
    float cutoff_hz{20000.0F};
    // Headphone rendering: the mono sound reaches each ear after its own delay and head shadow,
    // from `azimuth` (radians, 0 ahead, positive to the right) and `elevation` (radians, up).
    // left and right then carry the distance gain for each ear.
    bool binaural{};
    float azimuth{};
    float elevation{};
};

struct AudioVoiceInfo {
    std::uint64_t id{};
    std::string bus; // The bus it plays into, after unknown names fall back to Master.
    double position_seconds{};
    double duration_seconds{}; // Zero for a stream of unknown length.
    float left{}, right{};
    float send{}; // All sends together.
    float cutoff_hz{};
    float fade{}; // The scheduled fade's current gain.
    bool loop{}, paused{}, streaming{}, waiting{};
};

// A zone reverb's sound. `wet_db` sets the level its return comes back at.
struct AudioReverbParameters {
    double room_size{0.5};
    double damping{0.5};
    double wet_db{-6.0};
    double pre_delay_ms{10.0};
    friend bool operator==(const AudioReverbParameters&, const AudioReverbParameters&) = default;
};

// Post-fader levels, in decibels, with a short release so meters are readable.
struct AudioBusLevel {
    double peak_left_db{minimum_audio_volume_db};
    double peak_right_db{minimum_audio_volume_db};
    double rms_db{minimum_audio_volume_db};
};

// Deterministic software mixer: voices resampled from their clips or streams into a bus tree,
// rendered to interleaved stereo float at `audio_output_rate`. It owns no thread or device;
// whoever renders it decides when time passes, so tests render it offline and get the same
// samples every time.
class AudioMixer {
public:
    AudioMixer();
    void set_buses(const AudioSettings& settings);
    [[nodiscard]] const AudioSettings& buses() const { return settings_; }

    // Zero when the voice limit is reached or the clip is empty. `delay_frames` holds the voice
    // silent (without advancing) for that many output frames first, for starts on a beat.
    [[nodiscard]] std::uint64_t play(std::shared_ptr<const AudioClip> clip,
                                     const AudioVoiceTarget& target, double start_seconds = 0.0,
                                     std::uint64_t delay_frames = 0);
    [[nodiscard]] std::uint64_t play(std::shared_ptr<AudioStream> stream,
                                     const AudioVoiceTarget& target,
                                     std::uint64_t delay_frames = 0);
    bool update(std::uint64_t voice, const AudioVoiceTarget& target);
    // Fades out over a few milliseconds, then frees the voice.
    void stop(std::uint64_t voice);
    void stop_all();
    // Moves the voice's fade gain from where it is to `to` over `duration_frames`, starting
    // `delay_frames` from now; with `stop`, the voice ends when the fade does. Sample-accurate,
    // for crossfades between music tracks.
    bool fade(std::uint64_t voice, float to, std::uint64_t delay_frames, std::uint64_t duration_frames,
              bool stop);
    // Starts a voice's fade gain at zero, for fading in.
    bool set_fade(std::uint64_t voice, float gain);
    [[nodiscard]] bool active(std::uint64_t voice) const;
    [[nodiscard]] std::optional<AudioVoiceInfo> voice(std::uint64_t voice) const;
    [[nodiscard]] std::vector<AudioVoiceInfo> voices() const;
    [[nodiscard]] std::size_t voice_count() const { return voices_.size(); }
    // Frames since the voice last passed its start, at the clip's own rate (loops restart it).
    [[nodiscard]] std::optional<double> position_seconds(std::uint64_t voice) const;

    // Each slot's reverb; nothing turns a slot off, letting its tail ring out first.
    void set_reverbs(const std::array<std::optional<AudioReverbParameters>, maximum_reverb_slots>& reverbs);
    [[nodiscard]] const std::array<std::optional<AudioReverbParameters>, maximum_reverb_slots>& reverbs() const {
        return reverb_settings_;
    }

    // Mixes the next `output.size() / 2` frames, replacing what `output` held.
    void render(std::span<float> output);
    [[nodiscard]] std::vector<AudioBusLevel> levels() const;
    [[nodiscard]] std::uint64_t rendered_frames() const { return rendered_frames_; }

private:
    // The two-ear head model's per-voice state.
    struct Ears {
        std::array<float, 64> delay{};
        std::size_t write{};
        std::array<float, 2> itd{};        // Current delay per ear, in samples.
        std::array<float, 2> shadow_x{};   // Head shadow filter state per ear.
        std::array<float, 2> shadow_y{};
        std::array<float, 2> alpha{1.0F, 1.0F};
    };
    struct Fade {
        float gain{1.0F};
        float from{1.0F}, to{1.0F};
        std::uint64_t delay{}, elapsed{}, duration{};
        bool running{}, stop_at_end{};
    };
    struct Voice {
        std::uint64_t id{};
        std::shared_ptr<const AudioClip> clip;
        std::shared_ptr<AudioStream> stream;
        AudioVoiceTarget target;
        std::size_t bus{};
        double position{}; // In source frames: within the clip, or in the stream's running count.
        std::uint64_t start_delay{};
        float left{}, right{};
        std::array<float, maximum_reverb_slots> sends{};
        float cutoff_hz{20000.0F};
        std::array<float, 2> filtered{};
        Ears ears;
        Fade fade;
        bool stopping{};
        bool finished{};
        [[nodiscard]] std::uint32_t channels() const { return clip ? clip->channels : stream->channels(); }
        [[nodiscard]] std::uint32_t sample_rate() const { return clip ? clip->sample_rate : stream->sample_rate(); }
    };
    struct BusState {
        std::size_t parent{};
        float gain{};
        float target_gain{};
        std::vector<float> buffer;
        std::vector<float> send; // maximum_reverb_slots interleaved stereo blocks.
        std::vector<AudioEffect> effect_settings;
        std::vector<std::unique_ptr<AudioEffectProcessor>> effects;
        double peak_left{}, peak_right{}, mean_square{};
    };
    struct ReverbSlot {
        ReverbProcessor reverb{audio_output_rate};
        float gain{};
        std::vector<float> wet;
    };
    [[nodiscard]] std::size_t bus_index(std::string_view name) const;
    void refresh_bus_gains();
    std::uint64_t add_voice(Voice voice);
    void mix_voice(Voice& voice, std::span<float> destination, float* sends, std::size_t frames,
                   std::uint32_t active_slots);
    void configure_effects(BusState& bus, const std::vector<AudioEffect>& effects);
    [[nodiscard]] AudioVoiceInfo describe(const Voice& voice) const;

    AudioSettings settings_;
    std::vector<BusState> buses_;
    std::vector<Voice> voices_;
    std::uint64_t next_id_{1};
    std::uint64_t rendered_frames_{};
    std::array<std::optional<AudioReverbParameters>, maximum_reverb_slots> reverb_settings_;
    std::vector<ReverbSlot> reverb_slots_;
};

} // namespace relay
