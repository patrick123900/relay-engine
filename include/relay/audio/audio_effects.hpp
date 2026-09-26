#pragma once

#include "relay/audio/audio_settings.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace relay {

// Processes interleaved stereo in place at audio_output_rate. `configure` takes new parameters
// without clearing state, so tails ring on through parameter changes.
class AudioEffectProcessor {
public:
    virtual ~AudioEffectProcessor() = default;
    virtual void configure(const AudioEffect& effect) = 0;
    virtual void process(std::span<float> stereo) = 0;
};

[[nodiscard]] std::unique_ptr<AudioEffectProcessor> make_audio_effect(const AudioEffect& effect);

// Jezar's Freeverb (public domain): eight damped comb filters and four allpasses per channel, the
// right channel detuned for width, after a pre-delay. It produces the wet signal only.
class ReverbProcessor {
public:
    explicit ReverbProcessor(double sample_rate);
    // room_size and damping from 0 to 1; width 0 is mono, 1 full stereo.
    void configure(double room_size, double damping, double width, double pre_delay_ms);
    // Adds the reverberation of `input` into `output`, scaled by `gain`, over `frames` frames.
    void process(const float* input, float* output, std::size_t frames, float gain);
    void clear();

private:
    struct Comb {
        std::vector<float> buffer;
        std::size_t index{};
        float store{};
    };
    struct Allpass {
        std::vector<float> buffer;
        std::size_t index{};
    };
    std::array<std::array<Comb, 8>, 2> combs_;
    std::array<std::array<Allpass, 4>, 2> allpasses_;
    std::vector<float> pre_delay_;
    std::size_t pre_delay_index_{};
    std::size_t pre_delay_frames_{};
    double sample_rate_;
    float feedback_{0.84F};
    float damp1_{0.2F}, damp2_{0.8F};
    float wet1_{1.0F}, wet2_{0.0F};
};

} // namespace relay
