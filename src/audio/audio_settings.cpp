#include "relay/audio/audio_settings.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <set>

namespace relay {
namespace {

std::string number_json(const double value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return {buffer.data(), result.ptr};
}

struct ParameterRange {
    std::string_view name;
    double AudioEffect::*member;
    double minimum;
    double maximum;
};

constexpr std::array<ParameterRange, 19> parameter_ranges{{
    {"mix", &AudioEffect::mix, 0.0, 1.0},
    {"room_size", &AudioEffect::room_size, 0.0, 1.0},
    {"damping", &AudioEffect::damping, 0.0, 1.0},
    {"width", &AudioEffect::width, 0.0, 1.0},
    {"pre_delay_ms", &AudioEffect::pre_delay_ms, 0.0, 250.0},
    {"time_ms", &AudioEffect::time_ms, 1.0, 2000.0},
    {"feedback", &AudioEffect::feedback, 0.0, 0.95},
    {"low_db", &AudioEffect::low_db, -24.0, 24.0},
    {"mid_db", &AudioEffect::mid_db, -24.0, 24.0},
    {"mid_frequency", &AudioEffect::mid_frequency, 100.0, 10000.0},
    {"high_db", &AudioEffect::high_db, -24.0, 24.0},
    {"threshold_db", &AudioEffect::threshold_db, -60.0, 0.0},
    {"ratio", &AudioEffect::ratio, 1.0, 20.0},
    {"attack_ms", &AudioEffect::attack_ms, 0.1, 500.0},
    {"release_ms", &AudioEffect::release_ms, 1.0, 5000.0},
    {"makeup_db", &AudioEffect::makeup_db, 0.0, 24.0},
    {"ceiling_db", &AudioEffect::ceiling_db, -24.0, 0.0},
    {"cutoff_hz", &AudioEffect::cutoff_hz, 20.0, 20000.0},
    {"resonance", &AudioEffect::resonance, 0.1, 10.0},
}};

const ParameterRange* find_parameter(const std::string_view name) {
    for (const auto& range : parameter_ranges)
        if (range.name == name) return &range;
    return nullptr;
}

constexpr std::array<AudioEffect::Type, 7> effect_types{
    AudioEffect::Type::reverb,     AudioEffect::Type::delay,   AudioEffect::Type::eq,
    AudioEffect::Type::compressor, AudioEffect::Type::limiter, AudioEffect::Type::lowpass,
    AudioEffect::Type::highpass};

} // namespace

std::string_view audio_effect_type_name(const AudioEffect::Type type) {
    switch (type) {
    case AudioEffect::Type::reverb: return "reverb";
    case AudioEffect::Type::delay: return "delay";
    case AudioEffect::Type::eq: return "eq";
    case AudioEffect::Type::compressor: return "compressor";
    case AudioEffect::Type::limiter: return "limiter";
    case AudioEffect::Type::lowpass: return "lowpass";
    case AudioEffect::Type::highpass: return "highpass";
    }
    return "reverb";
}

std::optional<AudioEffect::Type> audio_effect_type_from_name(const std::string_view name) {
    for (const auto type : effect_types)
        if (audio_effect_type_name(type) == name) return type;
    return std::nullopt;
}

std::vector<std::string_view> audio_effect_parameters(const AudioEffect::Type type) {
    switch (type) {
    case AudioEffect::Type::reverb: return {"room_size", "damping", "width", "pre_delay_ms", "mix"};
    case AudioEffect::Type::delay: return {"time_ms", "feedback", "damping", "mix"};
    case AudioEffect::Type::eq: return {"low_db", "mid_db", "mid_frequency", "high_db"};
    case AudioEffect::Type::compressor:
        return {"threshold_db", "ratio", "attack_ms", "release_ms", "makeup_db"};
    case AudioEffect::Type::limiter: return {"ceiling_db", "release_ms"};
    case AudioEffect::Type::lowpass:
    case AudioEffect::Type::highpass: return {"cutoff_hz", "resonance"};
    }
    return {};
}

double* audio_effect_parameter(AudioEffect& effect, const std::string_view name) {
    const auto* range = find_parameter(name);
    return range ? &(effect.*(range->member)) : nullptr;
}

bool valid_audio_effect(const AudioEffect& effect, std::string& error) {
    if (!audio_effect_type_from_name(audio_effect_type_name(effect.type))) {
        error = "unknown effect type";
        return false;
    }
    for (const auto name : audio_effect_parameters(effect.type)) {
        const auto* range = find_parameter(name);
        const double value = effect.*(range->member);
        if (!std::isfinite(value) || value < range->minimum || value > range->maximum) {
            error = std::string(audio_effect_type_name(effect.type)) + ' ' + std::string(name) +
                    " must be from " + number_json(range->minimum) + " to " +
                    number_json(range->maximum);
            return false;
        }
    }
    return true;
}

AudioSettings default_audio_settings() {
    AudioSettings settings;
    AudioEffect limiter;
    limiter.type = AudioEffect::Type::limiter;
    settings.buses.push_back({std::string(master_audio_bus), "", 0.0, false, false, {limiter}});
    for (const char* name : {"Music", "SFX", "Ambience", "Voice"})
        settings.buses.push_back({name, std::string(master_audio_bus), 0.0, false, false, {}});
    return settings;
}

bool valid_audio_bus_name(const std::string_view name) {
    if (name.empty() || name.size() > 64U || name.front() == ' ' || name.back() == ' ') return false;
    return std::all_of(name.begin(), name.end(), [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == ' ';
    });
}

bool normalize_audio_settings(AudioSettings& settings, std::string& error) {
    if (settings.buses.empty() || settings.buses.size() > maximum_audio_buses) {
        error = "the mixer needs Master and at most " + std::to_string(maximum_audio_buses) +
                " buses";
        return false;
    }
    std::set<std::string, std::less<>> names;
    std::size_t masters = 0;
    for (const auto& bus : settings.buses) {
        if (!valid_audio_bus_name(bus.name)) {
            error = "invalid bus name \"" + bus.name +
                    "\": use letters, digits, spaces, '_' or '-', at most 64 characters";
            return false;
        }
        if (!names.insert(bus.name).second) {
            error = "two buses are named " + bus.name;
            return false;
        }
        if (!std::isfinite(bus.volume_db) || bus.volume_db < minimum_audio_volume_db ||
            bus.volume_db > maximum_audio_volume_db) {
            error = "bus " + bus.name + " volume must be from -80 to 24 dB";
            return false;
        }
        if (bus.effects.size() > maximum_bus_effects) {
            error = "bus " + bus.name + " has more than " + std::to_string(maximum_bus_effects) +
                    " effects";
            return false;
        }
        for (const auto& effect : bus.effects) {
            std::string problem;
            if (!valid_audio_effect(effect, problem)) {
                error = "bus " + bus.name + ": " + problem;
                return false;
            }
        }
        if (bus.name == master_audio_bus) {
            ++masters;
            if (!bus.parent.empty()) {
                error = "Master cannot have a parent";
                return false;
            }
        } else if (bus.parent.empty()) {
            error = "bus " + bus.name + " needs a parent";
            return false;
        }
    }
    if (masters != 1U) {
        error = "the mixer needs exactly one Master bus";
        return false;
    }
    // Place each bus after its parent. A bus that never becomes placeable has a missing parent
    // or sits in a cycle.
    std::vector<AudioBus> ordered;
    std::set<std::string, std::less<>> placed;
    auto remaining = settings.buses;
    while (!remaining.empty()) {
        const auto before = remaining.size();
        for (auto item = remaining.begin(); item != remaining.end();) {
            if (item->parent.empty() || placed.contains(item->parent)) {
                placed.insert(item->name);
                ordered.push_back(std::move(*item));
                item = remaining.erase(item);
            } else {
                ++item;
            }
        }
        if (remaining.size() == before) {
            const auto& bus = remaining.front();
            error = names.contains(bus.parent)
                        ? "bus " + bus.name + " is part of a parent cycle"
                        : "bus " + bus.name + " has a missing parent " + bus.parent;
            return false;
        }
    }
    settings.buses = std::move(ordered);
    return true;
}

std::string audio_settings_json(const AudioSettings& settings) {
    std::string output = "{\"buses\":[";
    for (std::size_t index = 0; index < settings.buses.size(); ++index) {
        const auto& bus = settings.buses[index];
        if (index) output += ',';
        output += "{\"name\":\"" + json_escape(bus.name) + "\",\"parent\":";
        output += bus.parent.empty() ? std::string("null") : '"' + json_escape(bus.parent) + '"';
        output += ",\"volume_db\":" + number_json(bus.volume_db) +
                  ",\"mute\":" + (bus.mute ? "true" : "false") +
                  ",\"solo\":" + (bus.solo ? "true" : "false") + ",\"effects\":[";
        for (std::size_t item = 0; item < bus.effects.size(); ++item) {
            const auto& effect = bus.effects[item];
            if (item) output += ',';
            output += "{\"type\":\"" + std::string(audio_effect_type_name(effect.type)) +
                      "\",\"enabled\":" + (effect.enabled ? "true" : "false");
            for (const auto name : audio_effect_parameters(effect.type))
                output += ",\"" + std::string(name) + "\":" +
                          number_json(effect.*(find_parameter(name)->member));
            output += '}';
        }
        output += "]}";
    }
    return output + "],\"spatialization\":\"" +
           (settings.spatialization == AudioSettings::Spatialization::binaural ? "binaural" : "stereo") +
           "\"}";
}

std::optional<AudioSettings> parse_audio_settings(const JsonValue& value, std::string& error) {
    const auto* object = value.object();
    if (object == nullptr) {
        error = "audio settings must be an object";
        return std::nullopt;
    }
    AudioSettings settings;
    for (const auto& [name, entry] : *object) {
        if (name == "spatialization") {
            const auto* mode = entry.string();
            if (!mode || (*mode != "stereo" && *mode != "binaural")) {
                error = "audio spatialization must be stereo or binaural";
                return std::nullopt;
            }
            settings.spatialization = *mode == "binaural" ? AudioSettings::Spatialization::binaural
                                                          : AudioSettings::Spatialization::stereo;
            continue;
        }
        if (name != "buses") {
            error = "unknown audio setting " + name;
            return std::nullopt;
        }
        const auto* buses = entry.array();
        if (buses == nullptr) {
            error = "audio buses must be an array";
            return std::nullopt;
        }
        for (const auto& item : *buses) {
            const auto* bus_object = item.object();
            if (bus_object == nullptr) {
                error = "each audio bus must be an object";
                return std::nullopt;
            }
            AudioBus bus;
            for (const auto& [key, field_value] : *bus_object) {
                if (key == "name" && field_value.string()) {
                    bus.name = *field_value.string();
                } else if (key == "parent" && (field_value.string() || field_value.is_null())) {
                    bus.parent = field_value.string() ? *field_value.string() : std::string{};
                } else if (key == "volume_db" && field_value.number()) {
                    bus.volume_db = *field_value.number();
                } else if (key == "mute" && field_value.boolean()) {
                    bus.mute = *field_value.boolean();
                } else if (key == "solo" && field_value.boolean()) {
                    bus.solo = *field_value.boolean();
                } else if (key == "effects" && field_value.array()) {
                    for (const auto& effect_value : *field_value.array()) {
                        const auto* effect_object = effect_value.object();
                        const auto* type_value = effect_object ? field(*effect_object, "type") : nullptr;
                        const auto type = type_value && type_value->string()
                                              ? audio_effect_type_from_name(*type_value->string())
                                              : std::nullopt;
                        if (!type) {
                            error = "bus " + bus.name + " has an effect of unknown type";
                            return std::nullopt;
                        }
                        AudioEffect effect;
                        effect.type = *type;
                        const auto parameters = audio_effect_parameters(*type);
                        for (const auto& [parameter, entry_value] : *effect_object) {
                            if (parameter == "type") continue;
                            if (parameter == "enabled" && entry_value.boolean()) {
                                effect.enabled = *entry_value.boolean();
                                continue;
                            }
                            auto* target = audio_effect_parameter(effect, parameter);
                            if (!target || !entry_value.number() ||
                                std::find(parameters.begin(), parameters.end(), parameter) ==
                                    parameters.end()) {
                                error = "invalid " + std::string(audio_effect_type_name(*type)) +
                                        " field " + parameter;
                                return std::nullopt;
                            }
                            *target = *entry_value.number();
                        }
                        bus.effects.push_back(effect);
                    }
                } else {
                    error = "invalid audio bus field " + key;
                    return std::nullopt;
                }
            }
            settings.buses.push_back(std::move(bus));
        }
    }
    if (!normalize_audio_settings(settings, error)) return std::nullopt;
    return settings;
}

double audio_db_to_gain(const double decibels) {
    if (!std::isfinite(decibels) || decibels <= minimum_audio_volume_db) return 0.0;
    return std::pow(10.0, decibels / 20.0);
}

double audio_gain_to_db(const double gain) {
    if (!(gain > 0.0)) return minimum_audio_volume_db;
    return std::max(minimum_audio_volume_db, 20.0 * std::log10(gain));
}

} // namespace relay
