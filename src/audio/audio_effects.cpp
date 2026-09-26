#include "relay/audio/audio_effects.hpp"
#include "relay/audio/audio_mixer.hpp"

#include <algorithm>
#include <cmath>

namespace relay {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double rate = audio_output_rate;

// A one-pole smoothing coefficient for a time constant in milliseconds.
float time_coefficient(const double milliseconds) {
    return static_cast<float>(std::exp(-1000.0 / (std::max(milliseconds, 0.01) * rate)));
}

// Robert Bristow-Johnson's cookbook biquads, in transposed direct form II.
struct Biquad {
    float b0{1}, b1{}, b2{}, a1{}, a2{};
    std::array<std::array<float, 2>, 2> state{};

    enum class Shape { lowpass, highpass, low_shelf, high_shelf, peak };
    void set(const Shape shape, const double frequency, const double q, const double gain_db) {
        const double f = std::clamp(frequency, 10.0, rate * 0.45);
        const double w = 2.0 * pi * f / rate;
        const double cosw = std::cos(w), sinw = std::sin(w);
        const double a = std::pow(10.0, gain_db / 40.0);
        const double alpha = sinw / (2.0 * std::max(q, 0.05));
        double nb0 = 1, nb1 = 0, nb2 = 0, na0 = 1, na1 = 0, na2 = 0;
        switch (shape) {
        case Shape::lowpass:
            nb0 = (1 - cosw) / 2; nb1 = 1 - cosw; nb2 = (1 - cosw) / 2;
            na0 = 1 + alpha; na1 = -2 * cosw; na2 = 1 - alpha;
            break;
        case Shape::highpass:
            nb0 = (1 + cosw) / 2; nb1 = -(1 + cosw); nb2 = (1 + cosw) / 2;
            na0 = 1 + alpha; na1 = -2 * cosw; na2 = 1 - alpha;
            break;
        case Shape::peak:
            nb0 = 1 + alpha * a; nb1 = -2 * cosw; nb2 = 1 - alpha * a;
            na0 = 1 + alpha / a; na1 = -2 * cosw; na2 = 1 - alpha / a;
            break;
        case Shape::low_shelf:
        case Shape::high_shelf: {
            // Shelf slope 1.
            const double shelf_alpha = sinw / 2.0 * std::sqrt(2.0);
            const double root = 2.0 * std::sqrt(a) * shelf_alpha;
            const double sign = shape == Shape::low_shelf ? 1.0 : -1.0;
            nb0 = a * ((a + 1) - sign * (a - 1) * cosw + root);
            nb1 = sign * 2 * a * ((a - 1) - sign * (a + 1) * cosw);
            nb2 = a * ((a + 1) - sign * (a - 1) * cosw - root);
            na0 = (a + 1) + sign * (a - 1) * cosw + root;
            na1 = -sign * 2 * ((a - 1) + sign * (a + 1) * cosw);
            na2 = (a + 1) + sign * (a - 1) * cosw - root;
            break;
        }
        }
        b0 = static_cast<float>(nb0 / na0);
        b1 = static_cast<float>(nb1 / na0);
        b2 = static_cast<float>(nb2 / na0);
        a1 = static_cast<float>(na1 / na0);
        a2 = static_cast<float>(na2 / na0);
    }
    float run(const float input, const std::size_t channel) {
        auto& z = state[channel];
        const float output = b0 * input + z[0];
        z[0] = b1 * input - a1 * output + z[1];
        z[1] = b2 * input - a2 * output;
        return output;
    }
};

class FilterEffect final : public AudioEffectProcessor {
public:
    void configure(const AudioEffect& effect) override {
        filter_.set(effect.type == AudioEffect::Type::lowpass ? Biquad::Shape::lowpass
                                                              : Biquad::Shape::highpass,
                    effect.cutoff_hz, effect.resonance, 0.0);
    }
    void process(const std::span<float> stereo) override {
        for (std::size_t sample = 0; sample < stereo.size(); ++sample)
            stereo[sample] = filter_.run(stereo[sample], sample & 1U);
    }

private:
    Biquad filter_;
};

class EqEffect final : public AudioEffectProcessor {
public:
    void configure(const AudioEffect& effect) override {
        low_.set(Biquad::Shape::low_shelf, 200.0, 0.707, effect.low_db);
        mid_.set(Biquad::Shape::peak, effect.mid_frequency, 1.0, effect.mid_db);
        high_.set(Biquad::Shape::high_shelf, 5000.0, 0.707, effect.high_db);
    }
    void process(const std::span<float> stereo) override {
        for (std::size_t sample = 0; sample < stereo.size(); ++sample) {
            const auto channel = sample & 1U;
            stereo[sample] = high_.run(mid_.run(low_.run(stereo[sample], channel), channel), channel);
        }
    }

private:
    Biquad low_, mid_, high_;
};

// Stereo-linked peak compressor: the louder channel sets the gain for both.
class CompressorEffect final : public AudioEffectProcessor {
public:
    void configure(const AudioEffect& effect) override {
        threshold_db_ = effect.threshold_db;
        slope_ = 1.0 - 1.0 / std::max(effect.ratio, 1.0);
        attack_ = time_coefficient(effect.attack_ms);
        release_ = time_coefficient(effect.release_ms);
        makeup_ = effect.makeup_db;
    }
    void process(const std::span<float> stereo) override {
        for (std::size_t frame = 0; frame + 1U < stereo.size(); frame += 2U) {
            const float peak = std::max(std::abs(stereo[frame]), std::abs(stereo[frame + 1U]));
            const float coefficient = peak > envelope_ ? attack_ : release_;
            envelope_ = coefficient * envelope_ + (1.0F - coefficient) * peak;
            const double level = 20.0 * std::log10(std::max(envelope_, 1e-9F));
            const double reduction = level > threshold_db_ ? (level - threshold_db_) * slope_ : 0.0;
            const auto gain = static_cast<float>(std::pow(10.0, (makeup_ - reduction) / 20.0));
            stereo[frame] *= gain;
            stereo[frame + 1U] *= gain;
        }
    }

private:
    double threshold_db_{-18}, slope_{0.75}, makeup_{};
    float attack_{}, release_{}, envelope_{};
};

// A peak limiter with instant attack: nothing leaves above the ceiling. Without lookahead, the
// first sample of a sudden peak is turned down abruptly, which a lookahead limiter would avoid.
class LimiterEffect final : public AudioEffectProcessor {
public:
    void configure(const AudioEffect& effect) override {
        ceiling_ = static_cast<float>(std::pow(10.0, effect.ceiling_db / 20.0));
        release_ = time_coefficient(effect.release_ms);
    }
    void process(const std::span<float> stereo) override {
        for (std::size_t frame = 0; frame + 1U < stereo.size(); frame += 2U) {
            const float peak = std::max(std::abs(stereo[frame]), std::abs(stereo[frame + 1U]));
            envelope_ = std::max(peak, envelope_ * release_);
            if (envelope_ <= ceiling_) continue;
            const float gain = ceiling_ / envelope_;
            stereo[frame] *= gain;
            stereo[frame + 1U] *= gain;
        }
    }

private:
    float ceiling_{1.0F}, release_{}, envelope_{};
};

// An echo with damped feedback, up to two seconds long.
class DelayEffect final : public AudioEffectProcessor {
public:
    DelayEffect() {
        for (auto& line : lines_) line.assign(static_cast<std::size_t>(rate * 2.0) + 1U, 0.0F);
    }
    void configure(const AudioEffect& effect) override {
        delay_ = std::clamp(static_cast<std::size_t>(effect.time_ms * rate / 1000.0), std::size_t{1},
                            lines_[0].size() - 1U);
        feedback_ = static_cast<float>(effect.feedback);
        damping_ = static_cast<float>(effect.damping);
        mix_ = static_cast<float>(effect.mix);
    }
    void process(const std::span<float> stereo) override {
        const auto size = lines_[0].size();
        for (std::size_t frame = 0; frame + 1U < stereo.size(); frame += 2U) {
            const auto read = (write_ + size - delay_) % size;
            for (std::size_t channel = 0; channel < 2U; ++channel) {
                const float echo = lines_[channel][read];
                damped_[channel] = echo * (1.0F - damping_) + damped_[channel] * damping_;
                const float input = stereo[frame + channel];
                lines_[channel][write_] = input + damped_[channel] * feedback_;
                stereo[frame + channel] = input * (1.0F - mix_) + echo * mix_;
            }
            write_ = (write_ + 1U) % size;
        }
    }

private:
    std::array<std::vector<float>, 2> lines_;
    std::array<float, 2> damped_{};
    std::size_t write_{}, delay_{1};
    float feedback_{}, damping_{}, mix_{};
};

class ReverbEffect final : public AudioEffectProcessor {
public:
    ReverbEffect() : reverb_(rate) {}
    void configure(const AudioEffect& effect) override {
        reverb_.configure(effect.room_size, effect.damping, effect.width, effect.pre_delay_ms);
        mix_ = static_cast<float>(effect.mix);
    }
    void process(const std::span<float> stereo) override {
        wet_.assign(stereo.size(), 0.0F);
        reverb_.process(stereo.data(), wet_.data(), stereo.size() / 2U, 1.0F);
        for (std::size_t sample = 0; sample < stereo.size(); ++sample)
            stereo[sample] = stereo[sample] * (1.0F - mix_) + wet_[sample] * mix_;
    }

private:
    ReverbProcessor reverb_;
    std::vector<float> wet_;
    float mix_{};
};

} // namespace

std::unique_ptr<AudioEffectProcessor> make_audio_effect(const AudioEffect& effect) {
    std::unique_ptr<AudioEffectProcessor> processor;
    switch (effect.type) {
    case AudioEffect::Type::reverb: processor = std::make_unique<ReverbEffect>(); break;
    case AudioEffect::Type::delay: processor = std::make_unique<DelayEffect>(); break;
    case AudioEffect::Type::eq: processor = std::make_unique<EqEffect>(); break;
    case AudioEffect::Type::compressor: processor = std::make_unique<CompressorEffect>(); break;
    case AudioEffect::Type::limiter: processor = std::make_unique<LimiterEffect>(); break;
    case AudioEffect::Type::lowpass:
    case AudioEffect::Type::highpass: processor = std::make_unique<FilterEffect>(); break;
    }
    processor->configure(effect);
    return processor;
}

ReverbProcessor::ReverbProcessor(const double sample_rate) : sample_rate_(sample_rate) {
    // Freeverb's tunings are in samples at 44.1 kHz; the right channel is 23 samples longer.
    constexpr std::array<int, 8> comb_tuning{1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    constexpr std::array<int, 4> allpass_tuning{556, 441, 341, 225};
    constexpr int spread = 23;
    const double scale = sample_rate / 44100.0;
    for (std::size_t channel = 0; channel < 2U; ++channel) {
        const int offset = channel ? spread : 0;
        for (std::size_t index = 0; index < 8U; ++index)
            combs_[channel][index].buffer.assign(
                static_cast<std::size_t>((comb_tuning[index] + offset) * scale), 0.0F);
        for (std::size_t index = 0; index < 4U; ++index)
            allpasses_[channel][index].buffer.assign(
                static_cast<std::size_t>((allpass_tuning[index] + offset) * scale), 0.0F);
    }
    pre_delay_.assign(static_cast<std::size_t>(sample_rate * 0.25) + 1U, 0.0F);
    configure(0.5, 0.5, 1.0, 0.0);
}

void ReverbProcessor::configure(const double room_size, const double damping, const double width,
                                const double pre_delay_ms) {
    feedback_ = static_cast<float>(std::clamp(room_size, 0.0, 1.0) * 0.28 + 0.7);
    damp1_ = static_cast<float>(std::clamp(damping, 0.0, 1.0) * 0.4);
    damp2_ = 1.0F - damp1_;
    const auto spread = static_cast<float>(std::clamp(width, 0.0, 1.0));
    wet1_ = spread / 2.0F + 0.5F;
    wet2_ = (1.0F - spread) / 2.0F;
    pre_delay_frames_ = std::min(static_cast<std::size_t>(std::max(pre_delay_ms, 0.0) *
                                                          sample_rate_ / 1000.0),
                                 pre_delay_.size() - 1U);
}

void ReverbProcessor::clear() {
    for (auto& channel : combs_)
        for (auto& comb : channel) {
            std::fill(comb.buffer.begin(), comb.buffer.end(), 0.0F);
            comb.store = 0.0F;
        }
    for (auto& channel : allpasses_)
        for (auto& allpass : channel) std::fill(allpass.buffer.begin(), allpass.buffer.end(), 0.0F);
    std::fill(pre_delay_.begin(), pre_delay_.end(), 0.0F);
}

void ReverbProcessor::process(const float* input, float* output, const std::size_t frames,
                              const float gain) {
    // Freeverb's input gain (0.015) and wet scale (3).
    constexpr float input_gain = 0.015F * 3.0F;
    const auto size = pre_delay_.size();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        pre_delay_[pre_delay_index_] = (input[frame * 2U] + input[frame * 2U + 1U]) * input_gain;
        const float mono = pre_delay_[(pre_delay_index_ + size - pre_delay_frames_) % size];
        pre_delay_index_ = (pre_delay_index_ + 1U) % size;
        std::array<float, 2> out{};
        for (std::size_t channel = 0; channel < 2U; ++channel) {
            float sum = 0.0F;
            for (auto& comb : combs_[channel]) {
                const float value = comb.buffer[comb.index];
                comb.store = value * damp2_ + comb.store * damp1_;
                comb.buffer[comb.index] = mono + comb.store * feedback_;
                comb.index = (comb.index + 1U) % comb.buffer.size();
                sum += value;
            }
            for (auto& allpass : allpasses_[channel]) {
                const float buffered = allpass.buffer[allpass.index];
                allpass.buffer[allpass.index] = sum + buffered * 0.5F;
                allpass.index = (allpass.index + 1U) % allpass.buffer.size();
                sum = buffered - sum;
            }
            out[channel] = sum;
        }
        output[frame * 2U] += gain * (out[0] * wet1_ + out[1] * wet2_);
        output[frame * 2U + 1U] += gain * (out[1] * wet1_ + out[0] * wet2_);
    }
}

} // namespace relay
