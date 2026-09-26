#include "relay/audio/audio_mixer.hpp"

#include <algorithm>
#include <cmath>

namespace relay {
namespace {

// Gains, pauses and stops ramp over at most one block, about 5 ms.
constexpr std::size_t block_frames = 256U;
constexpr std::size_t block_samples = block_frames * 2U;
// Meters fall back with a 300 ms time constant.
constexpr double meter_release_seconds = 0.3;
constexpr float open_cutoff_hz = 20000.0F;
constexpr double pi = 3.14159265358979323846;

// The head model (Brown and Duda, "A structural model for binaural sound synthesis", 1998): a
// rigid sphere of radius 8.75 cm, the ear's shadow a one-pole, one-zero filter whose high-frequency
// gain alpha depends on the angle between the ear and the sound.
constexpr double head_radius = 0.0875;
constexpr double speed_of_sound = 343.0;
constexpr double shadow_beta = 2.0 * speed_of_sound / head_radius;

double level_db(const double linear) { return audio_gain_to_db(linear); }

// A one-pole low-pass coefficient for a cutoff frequency.
float lowpass_coefficient(const float cutoff_hz) {
    return static_cast<float>(1.0 - std::exp(-2.0 * pi * std::max(cutoff_hz, 10.0F) / audio_output_rate));
}

// For an ear `theta` radians from the sound: the head shadow's high-frequency gain, and the
// sound's extra travel time to that ear in samples (zero when the sound faces the ear).
float shadow_alpha(const double theta) {
    constexpr double alpha_min = 0.1, theta_min = 150.0 * pi / 180.0;
    return static_cast<float>(1.0 + alpha_min / 2.0 +
                              (1.0 - alpha_min / 2.0) * std::cos(theta / theta_min * pi));
}
float ear_delay(const double theta) {
    const double seconds = theta < pi / 2.0 ? head_radius / speed_of_sound * (1.0 - std::cos(theta))
                                            : head_radius / speed_of_sound * (theta - pi / 2.0 + 1.0);
    return static_cast<float>(seconds * audio_output_rate);
}

// The angle between each ear (left, right) and a sound at `azimuth` and `elevation`.
std::array<double, 2> ear_angles(const double azimuth, const double elevation) {
    const double x = std::sin(azimuth) * std::cos(elevation);
    return {std::acos(std::clamp(-x, -1.0, 1.0)), std::acos(std::clamp(x, -1.0, 1.0))};
}

} // namespace

AudioMixer::AudioMixer() {
    reverb_slots_.resize(maximum_reverb_slots);
    for (auto& slot : reverb_slots_) slot.wet.assign(block_samples, 0.0F);
    set_buses(default_audio_settings());
}

void AudioMixer::set_buses(const AudioSettings& settings) {
    auto normalized = settings;
    std::string error;
    if (!normalize_audio_settings(normalized, error)) normalized = default_audio_settings();
    const bool same_layout =
        normalized.buses.size() == settings_.buses.size() &&
        std::equal(normalized.buses.begin(), normalized.buses.end(), settings_.buses.begin(),
                   [](const AudioBus& a, const AudioBus& b) {
                       return a.name == b.name && a.parent == b.parent;
                   });
    std::vector<bool> fresh;
    if (!same_layout) {
        // A bus that keeps its name keeps its effects, and their state, so tails ring on.
        std::vector<BusState> previous = std::move(buses_);
        fresh.assign(normalized.buses.size(), true);
        const auto old_settings = settings_;
        buses_.clear();
        buses_.resize(normalized.buses.size());
        for (std::size_t index = 0; index < normalized.buses.size(); ++index) {
            auto& bus = buses_[index];
            bus.buffer.assign(block_samples, 0.0F);
            bus.send.assign(block_samples * maximum_reverb_slots, 0.0F);
            for (std::size_t old = 0; old < old_settings.buses.size(); ++old) {
                if (old_settings.buses[old].name != normalized.buses[index].name) continue;
                bus.effects = std::move(previous[old].effects);
                bus.effect_settings = std::move(previous[old].effect_settings);
                bus.gain = previous[old].gain;
                fresh[index] = false;
            }
        }
    }
    settings_ = std::move(normalized);
    for (std::size_t index = 0; index < settings_.buses.size(); ++index) {
        buses_[index].parent = bus_index(settings_.buses[index].parent);
        configure_effects(buses_[index], settings_.buses[index].effects);
    }
    if (!same_layout)
        for (auto& voice : voices_) voice.bus = bus_index(voice.target.bus);
    refresh_bus_gains();
    // A new bus starts at its volume rather than fading in.
    for (std::size_t index = 0; index < fresh.size(); ++index)
        if (fresh[index]) buses_[index].gain = buses_[index].target_gain;
}

void AudioMixer::configure_effects(BusState& bus, const std::vector<AudioEffect>& effects) {
    const bool same_types =
        effects.size() == bus.effect_settings.size() &&
        std::equal(effects.begin(), effects.end(), bus.effect_settings.begin(),
                   [](const AudioEffect& a, const AudioEffect& b) { return a.type == b.type; });
    if (!same_types) {
        bus.effects.clear();
        for (const auto& effect : effects) bus.effects.push_back(make_audio_effect(effect));
    } else {
        for (std::size_t index = 0; index < effects.size(); ++index)
            if (!(effects[index] == bus.effect_settings[index]))
                bus.effects[index]->configure(effects[index]);
    }
    bus.effect_settings = effects;
}

std::size_t AudioMixer::bus_index(const std::string_view name) const {
    for (std::size_t index = 0; index < settings_.buses.size(); ++index)
        if (settings_.buses[index].name == name) return index;
    return 0; // Master; normalize_audio_settings puts it first.
}

void AudioMixer::refresh_bus_gains() {
    const auto count = settings_.buses.size();
    // A bus is heard while nothing is soloed, or when it, an ancestor or a descendant is soloed.
    std::vector<bool> soloed_line(count, false);
    bool any_solo = false;
    for (std::size_t index = 0; index < count; ++index) {
        if (!settings_.buses[index].solo) continue;
        any_solo = true;
        for (std::size_t current = index;; current = buses_[current].parent) {
            soloed_line[current] = true;
            if (current == 0U) break;
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        bool heard = !any_solo || soloed_line[index];
        for (std::size_t current = index; !heard && current != 0U;) {
            current = buses_[current].parent;
            heard = settings_.buses[current].solo;
        }
        const auto& bus = settings_.buses[index];
        buses_[index].target_gain =
            heard && !bus.mute ? static_cast<float>(audio_db_to_gain(bus.volume_db)) : 0.0F;
    }
}

std::uint64_t AudioMixer::add_voice(Voice voice) {
    if (voices_.size() >= maximum_audio_voices) return 0;
    voice.id = next_id_++;
    voice.bus = bus_index(voice.target.bus);
    // Start at full level rather than ramping, so attacks stay sharp.
    const bool silent = voice.target.paused;
    voice.left = silent ? 0.0F : voice.target.left;
    voice.right = silent ? 0.0F : voice.target.right;
    if (!silent) voice.sends = voice.target.sends;
    voice.cutoff_hz = voice.target.cutoff_hz;
    if (voice.target.binaural) {
        const auto theta = ear_angles(voice.target.azimuth, voice.target.elevation);
        voice.ears.itd = {ear_delay(theta[0]), ear_delay(theta[1])};
    }
    voices_.push_back(std::move(voice));
    return voices_.back().id;
}

std::uint64_t AudioMixer::play(std::shared_ptr<const AudioClip> clip,
                               const AudioVoiceTarget& target, const double start_seconds,
                               const std::uint64_t delay_frames) {
    if (!clip || clip->frames() == 0U) return 0;
    Voice voice;
    voice.target = target;
    voice.position = std::clamp(start_seconds, 0.0, clip->duration_seconds()) * clip->sample_rate;
    voice.start_delay = delay_frames;
    voice.clip = std::move(clip);
    return add_voice(std::move(voice));
}

std::uint64_t AudioMixer::play(std::shared_ptr<AudioStream> stream, const AudioVoiceTarget& target,
                               const std::uint64_t delay_frames) {
    if (!stream) return 0;
    Voice voice;
    voice.target = target;
    voice.start_delay = delay_frames;
    voice.stream = std::move(stream);
    voice.stream->set_loop(target.loop);
    return add_voice(std::move(voice));
}

bool AudioMixer::update(const std::uint64_t id, const AudioVoiceTarget& target) {
    for (auto& voice : voices_) {
        if (voice.id != id || voice.stopping) continue;
        voice.target = target;
        voice.bus = bus_index(target.bus);
        if (voice.stream) voice.stream->set_loop(target.loop);
        return true;
    }
    return false;
}

void AudioMixer::stop(const std::uint64_t id) {
    for (auto& voice : voices_)
        if (voice.id == id) voice.stopping = true;
}

void AudioMixer::stop_all() {
    for (auto& voice : voices_) voice.stopping = true;
}

bool AudioMixer::fade(const std::uint64_t id, const float to, const std::uint64_t delay_frames,
                      const std::uint64_t duration_frames, const bool stop_at_end) {
    for (auto& voice : voices_) {
        if (voice.id != id || voice.stopping) continue;
        voice.fade.from = voice.fade.gain;
        voice.fade.to = to;
        voice.fade.delay = delay_frames;
        voice.fade.elapsed = 0;
        voice.fade.duration = std::max<std::uint64_t>(duration_frames, 1U);
        voice.fade.running = true;
        voice.fade.stop_at_end = stop_at_end;
        return true;
    }
    return false;
}

bool AudioMixer::set_fade(const std::uint64_t id, const float gain) {
    for (auto& voice : voices_) {
        if (voice.id != id) continue;
        voice.fade = Fade{};
        voice.fade.gain = voice.fade.from = voice.fade.to = gain;
        return true;
    }
    return false;
}

bool AudioMixer::active(const std::uint64_t id) const {
    return std::any_of(voices_.begin(), voices_.end(), [id](const Voice& voice) {
        return voice.id == id && !voice.stopping;
    });
}

AudioVoiceInfo AudioMixer::describe(const Voice& voice) const {
    AudioVoiceInfo info;
    info.id = voice.id;
    info.bus = settings_.buses[voice.bus].name;
    const double rate = voice.sample_rate();
    if (voice.clip) {
        info.position_seconds = voice.position / rate;
        info.duration_seconds = voice.clip->duration_seconds();
    } else {
        const auto track = voice.stream->track_frames();
        info.duration_seconds = static_cast<double>(track) / rate;
        info.position_seconds = (track ? std::fmod(voice.position, static_cast<double>(track))
                                       : voice.position) / rate;
        info.streaming = true;
    }
    info.left = voice.left;
    info.right = voice.right;
    for (const float send : voice.sends) info.send += send;
    info.cutoff_hz = voice.cutoff_hz;
    info.fade = voice.fade.gain;
    info.loop = voice.target.loop;
    info.paused = voice.target.paused;
    info.waiting = voice.start_delay > 0U;
    return info;
}

std::optional<AudioVoiceInfo> AudioMixer::voice(const std::uint64_t id) const {
    for (const auto& voice : voices_)
        if (voice.id == id) return describe(voice);
    return std::nullopt;
}

std::vector<AudioVoiceInfo> AudioMixer::voices() const {
    std::vector<AudioVoiceInfo> result;
    for (const auto& voice : voices_)
        if (!voice.stopping) result.push_back(describe(voice));
    return result;
}

std::optional<double> AudioMixer::position_seconds(const std::uint64_t id) const {
    const auto info = voice(id);
    if (!info) return std::nullopt;
    return info->position_seconds;
}

void AudioMixer::set_reverbs(
    const std::array<std::optional<AudioReverbParameters>, maximum_reverb_slots>& reverbs) {
    reverb_settings_ = reverbs;
}

void AudioMixer::mix_voice(Voice& voice, const std::span<float> destination, float* const sends,
                           const std::size_t frames, const std::uint32_t active_slots) {
    std::size_t first = 0;
    if (voice.start_delay > 0U) {
        if (voice.stopping) {
            voice.finished = true;
            return;
        }
        const auto wait = std::min<std::uint64_t>(voice.start_delay, frames);
        voice.start_delay -= wait;
        first = static_cast<std::size_t>(wait);
        if (first == frames) return;
    }
    const bool silent_target = voice.stopping || voice.target.paused;
    const float target_left = silent_target ? 0.0F : voice.target.left;
    const float target_right = silent_target ? 0.0F : voice.target.right;
    std::array<float, maximum_reverb_slots> target_sends{};
    if (!silent_target) target_sends = voice.target.sends;
    // A paused voice that has faded out holds its place without advancing.
    const bool quiet = voice.left == 0.0F && voice.right == 0.0F &&
                       std::all_of(voice.sends.begin(), voice.sends.end(), [](float s) { return s == 0.0F; });
    if (voice.target.paused && !voice.stopping && quiet) return;
    const auto channels = voice.channels();
    const double step = std::max(0.0, voice.target.rate) * voice.sample_rate() / audio_output_rate;
    const auto count = static_cast<float>(frames - first);
    const float left_step = (target_left - voice.left) / count;
    const float right_step = (target_right - voice.right) / count;
    std::array<float, maximum_reverb_slots> send_steps{};
    for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot)
        send_steps[slot] = (target_sends[slot] - voice.sends[slot]) / count;
    // The filter sweeps between cutoffs over the block, like the gains.
    const bool filtering = voice.cutoff_hz < open_cutoff_hz || voice.target.cutoff_hz < open_cutoff_hz;
    const float from_coefficient = lowpass_coefficient(voice.cutoff_hz);
    const float coefficient_step = (lowpass_coefficient(voice.target.cutoff_hz) - from_coefficient) / count;
    // Binaural: each ear's delay ramps across the block; its shadow filter is set once per block.
    const bool binaural = voice.target.binaural;
    std::array<float, 2> itd_step{}, ear_b0{}, ear_b1{}, ear_a1{};
    if (binaural) {
        const auto theta = ear_angles(voice.target.azimuth, voice.target.elevation);
        constexpr double k = 2.0 * audio_output_rate;
        for (std::size_t ear = 0; ear < 2U; ++ear) {
            itd_step[ear] = (ear_delay(theta[ear]) - voice.ears.itd[ear]) / count;
            const double alpha = shadow_alpha(theta[ear]);
            ear_b0[ear] = static_cast<float>((shadow_beta + alpha * k) / (shadow_beta + k));
            ear_b1[ear] = static_cast<float>((shadow_beta - alpha * k) / (shadow_beta + k));
            ear_a1[ear] = static_cast<float>((shadow_beta - k) / (shadow_beta + k));
        }
    }
    const auto read_clip = [&](std::size_t frame, const std::size_t channel) -> float {
        const auto clip_frames = voice.clip->frames();
        if (frame >= clip_frames) {
            if (!voice.target.loop) return 0.0F;
            frame %= clip_frames;
        }
        return voice.clip->samples[frame * channels + channel];
    };
    float left_gain = voice.left;
    float right_gain = voice.right;
    auto send_gains = voice.sends;
    float coefficient = from_coefficient;
    auto& fade = voice.fade;
    for (std::size_t frame = first; frame < frames; ++frame) {
        bool starved = false;
        if (voice.clip) {
            const auto clip_frames = static_cast<double>(voice.clip->frames());
            if (voice.position >= clip_frames) {
                if (!voice.target.loop) {
                    voice.finished = true;
                    break;
                }
                voice.position = std::fmod(voice.position, clip_frames);
            }
        } else {
            const auto written = voice.stream->written();
            const auto at = static_cast<std::uint64_t>(voice.position);
            if (at >= written && voice.stream->ended()) {
                voice.finished = true;
                break;
            }
            // Not decoded yet: wait in silence rather than skip ahead.
            starved = at + 1U >= written && !voice.stream->ended();
        }
        const auto index = static_cast<std::size_t>(voice.position);
        const auto fraction = static_cast<float>(voice.position - static_cast<double>(index));
        const auto read = [&](const std::size_t channel) {
            if (voice.clip) {
                const float a = read_clip(index, channel);
                return a + (read_clip(index + 1U, channel) - a) * fraction;
            }
            const auto c = static_cast<std::uint32_t>(channel);
            const float a = voice.stream->sample(index, c);
            return a + (voice.stream->sample(index + 1U, c) - a) * fraction;
        };
        float left = starved ? 0.0F : read(0);
        float right = starved ? 0.0F : (channels > 1U ? read(1) : left);
        if ((voice.target.downmix || binaural) && channels > 1U) left = right = 0.5F * (left + right);
        if (filtering) {
            coefficient += coefficient_step;
            voice.filtered[0] += coefficient * (left - voice.filtered[0]);
            voice.filtered[1] += coefficient * (right - voice.filtered[1]);
            left = voice.filtered[0];
            right = voice.filtered[1];
        }
        // The scheduled fade, sample by sample.
        if (fade.running) {
            if (fade.delay > 0U) {
                --fade.delay;
            } else {
                ++fade.elapsed;
                fade.gain = fade.from + (fade.to - fade.from) * static_cast<float>(fade.elapsed) /
                                            static_cast<float>(fade.duration);
                if (fade.elapsed >= fade.duration) {
                    fade.gain = fade.to;
                    fade.running = false;
                    if (fade.stop_at_end) voice.finished = true;
                }
            }
        }
        left *= fade.gain;
        right *= fade.gain;
        const float mono = 0.5F * (left + right);
        if (binaural) {
            auto& ears = voice.ears;
            const auto size = ears.delay.size();
            ears.delay[ears.write] = mono;
            std::array<float, 2> heard{};
            for (std::size_t ear = 0; ear < 2U; ++ear) {
                ears.itd[ear] += itd_step[ear];
                const float delay = std::clamp(ears.itd[ear], 0.0F, 62.0F);
                const auto whole = static_cast<std::size_t>(delay);
                const float part = delay - static_cast<float>(whole);
                const float a = ears.delay[(ears.write + size - whole) % size];
                const float b = ears.delay[(ears.write + size - whole - 1U) % size];
                const float input = a + (b - a) * part;
                const float output = ear_b0[ear] * input + ear_b1[ear] * ears.shadow_x[ear] -
                                     ear_a1[ear] * ears.shadow_y[ear];
                ears.shadow_x[ear] = input;
                ears.shadow_y[ear] = output;
                heard[ear] = output;
            }
            ears.write = (ears.write + 1U) % size;
            left = heard[0];
            right = heard[1];
        }
        left_gain += left_step;
        right_gain += right_step;
        destination[frame * 2U] += left * left_gain;
        destination[frame * 2U + 1U] += right * right_gain;
        for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot) {
            send_gains[slot] += send_steps[slot];
            if (!(active_slots & (1U << slot)) || send_gains[slot] == 0.0F) continue;
            // The reverb hears the sound centred; its own stereo spreads it.
            const float sent = mono * send_gains[slot];
            sends[slot * block_samples + frame * 2U] += sent;
            sends[slot * block_samples + frame * 2U + 1U] += sent;
        }
        if (!starved) voice.position += step;
        if (voice.finished) break;
    }
    voice.left = target_left;
    voice.right = target_right;
    voice.sends = target_sends;
    voice.cutoff_hz = voice.target.cutoff_hz;
    if (!filtering) voice.filtered = {};
    if (voice.stream) voice.stream->consumed = static_cast<std::uint64_t>(voice.position);
    if (voice.stopping) voice.finished = true;
}

void AudioMixer::render(const std::span<float> output) {
    const auto total_frames = output.size() / 2U;
    const double release =
        std::exp(-static_cast<double>(block_frames) / (meter_release_seconds * audio_output_rate));
    // Slots in use: configured, or still ringing out after their zone went away.
    std::uint32_t active_slots = 0;
    std::array<float, maximum_reverb_slots> slot_targets{};
    for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot) {
        if (const auto& settings = reverb_settings_[slot]) {
            reverb_slots_[slot].reverb.configure(settings->room_size, settings->damping, 1.0,
                                                 settings->pre_delay_ms);
            slot_targets[slot] = static_cast<float>(audio_db_to_gain(settings->wet_db));
        }
        if (reverb_settings_[slot] || reverb_slots_[slot].gain > 0.0F) active_slots |= 1U << slot;
    }
    for (std::size_t offset = 0; offset < total_frames; offset += block_frames) {
        const auto frames = std::min(block_frames, total_frames - offset);
        const auto samples = frames * 2U;
        for (auto& bus : buses_) {
            std::fill(bus.buffer.begin(), bus.buffer.end(), 0.0F);
            if (active_slots) std::fill(bus.send.begin(), bus.send.end(), 0.0F);
        }
        for (auto& voice : voices_)
            mix_voice(voice, std::span<float>(buses_[voice.bus].buffer.data(), samples),
                      buses_[voice.bus].send.data(), frames, active_slots);
        std::erase_if(voices_, [](const Voice& voice) { return voice.finished; });
        // Parents come before children, so walking backwards folds each bus into its parent
        // after everything beneath it has arrived.
        for (std::size_t index = buses_.size(); index-- > 0U;) {
            auto& bus = buses_[index];
            if (index == 0U) {
                // Each zone's reverb returns into Master, before Master's own effects.
                for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot) {
                    if (!(active_slots & (1U << slot))) continue;
                    auto& reverb = reverb_slots_[slot];
                    std::fill(reverb.wet.begin(), reverb.wet.end(), 0.0F);
                    reverb.reverb.process(bus.send.data() + slot * block_samples, reverb.wet.data(),
                                          frames, 1.0F);
                    const float from = reverb.gain;
                    const float gain_step = (slot_targets[slot] - from) / static_cast<float>(frames);
                    float gain = from;
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        gain += gain_step;
                        bus.buffer[frame * 2U] += reverb.wet[frame * 2U] * gain;
                        bus.buffer[frame * 2U + 1U] += reverb.wet[frame * 2U + 1U] * gain;
                    }
                    reverb.gain = slot_targets[slot];
                    // A slot let go is cleared, so its old tail cannot return with a new zone.
                    if (from > 0.0F && reverb.gain == 0.0F) reverb.reverb.clear();
                }
            }
            const auto block = std::span<float>(bus.buffer.data(), samples);
            for (std::size_t effect = 0; effect < bus.effects.size(); ++effect)
                if (bus.effect_settings[effect].enabled) bus.effects[effect]->process(block);
            const float gain_step = (bus.target_gain - bus.gain) / static_cast<float>(frames);
            float gain = bus.gain;
            double peak_left = 0.0, peak_right = 0.0, square_sum = 0.0;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                gain += gain_step;
                auto& left = bus.buffer[frame * 2U];
                auto& right = bus.buffer[frame * 2U + 1U];
                left *= gain;
                right *= gain;
                for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot) {
                    if (!(active_slots & (1U << slot))) continue;
                    bus.send[slot * block_samples + frame * 2U] *= gain;
                    bus.send[slot * block_samples + frame * 2U + 1U] *= gain;
                }
                peak_left = std::max(peak_left, static_cast<double>(std::abs(left)));
                peak_right = std::max(peak_right, static_cast<double>(std::abs(right)));
                square_sum += 0.5 * (static_cast<double>(left) * left +
                                     static_cast<double>(right) * right);
            }
            bus.gain = bus.target_gain;
            bus.peak_left = std::max(peak_left, bus.peak_left * release);
            bus.peak_right = std::max(peak_right, bus.peak_right * release);
            bus.mean_square = bus.mean_square * release +
                              (1.0 - release) * square_sum / static_cast<double>(frames);
            if (index == 0U) continue;
            auto& parent = buses_[bus.parent];
            for (std::size_t sample = 0; sample < samples; ++sample)
                parent.buffer[sample] += bus.buffer[sample];
            for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot) {
                if (!(active_slots & (1U << slot))) continue;
                for (std::size_t sample = 0; sample < samples; ++sample)
                    parent.send[slot * block_samples + sample] += bus.send[slot * block_samples + sample];
            }
        }
        // Master's limiter keeps levels in range; the clamp only guards against a bus without one.
        const auto& master = buses_.front().buffer;
        for (std::size_t sample = 0; sample < samples; ++sample)
            output[offset * 2U + sample] = std::clamp(master[sample], -1.0F, 1.0F);
        rendered_frames_ += frames;
        // A slot that has rung out stops costing anything.
        for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot)
            if (!reverb_settings_[slot] && reverb_slots_[slot].gain == 0.0F) active_slots &= ~(1U << slot);
    }
}

std::vector<AudioBusLevel> AudioMixer::levels() const {
    std::vector<AudioBusLevel> result;
    result.reserve(buses_.size());
    for (const auto& bus : buses_)
        result.push_back({level_db(bus.peak_left), level_db(bus.peak_right),
                          level_db(std::sqrt(bus.mean_square))});
    return result;
}

} // namespace relay
