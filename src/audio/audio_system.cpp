#include "relay/audio/audio_system.hpp"
#include "relay/core/json.hpp"
#include "relay/editor/editor_math.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <numeric>
#include <vector>

namespace relay {
namespace {

constexpr double speed_of_sound = 343.0;
constexpr double half_pi = 1.57079632679489661923;

std::string number_json(const double value) {
    if (!std::isfinite(value)) return "0";
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return {buffer.data(), result.ptr};
}

Vec3 subtract(const Vec3 a, const Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scaled(const Vec3 a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const Vec3 a, const Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(const Vec3 a, const Vec3 fallback) {
    const auto size = length(a);
    return size > 1e-9 ? scaled(a, 1.0 / size) : fallback;
}

// The entity's world matrix, including keyframed transforms, as the renderer draws it.
std::optional<EditorMatrix> world_matrix(const Scene& scene, const Entity entity) {
    std::vector<const EntityRecord*> chain;
    for (auto current = entity; current.valid();) {
        const auto* record = scene.get(current);
        if (!record || chain.size() > 4096U) return std::nullopt;
        chain.push_back(record);
        current = record->parent;
    }
    auto world = editor_identity();
    for (auto item = chain.rbegin(); item != chain.rend(); ++item) {
        const auto* record = *item;
        const auto transform =
            record->transform_animation && !record->transform_animation->keys.empty()
                ? sample_transform_animation(*record->transform_animation, record->transform)
                : record->transform;
        world = editor_multiply(world, editor_compose(transform.position,
                                                      transform.rotation_degrees, transform.scale));
    }
    return world;
}

Vec3 matrix_column(const EditorMatrix& matrix, const std::size_t column) {
    return {matrix[column * 4U], matrix[column * 4U + 1U], matrix[column * 4U + 2U]};
}

Vec3 transform_point(const EditorMatrix& matrix, const Vec3 point) {
    return {matrix[0] * point.x + matrix[4] * point.y + matrix[8] * point.z + matrix[12],
            matrix[1] * point.x + matrix[5] * point.y + matrix[9] * point.z + matrix[13],
            matrix[2] * point.x + matrix[6] * point.y + matrix[10] * point.z + matrix[14]};
}

// Whether `a` is `b`, or one contains the other.
bool related(const Scene& scene, const Entity a, const Entity b) {
    const auto contains = [&](const Entity ancestor, Entity current) {
        for (std::size_t depth = 0; current.valid() && depth < 4096U; ++depth) {
            if (current == ancestor) return true;
            const auto* record = scene.get(current);
            if (!record) return false;
            current = record->parent;
        }
        return false;
    };
    return contains(a, b) || contains(b, a);
}

void append_vec3(std::string& json, const Vec3 value) {
    json += '[' + number_json(value.x) + ',' + number_json(value.y) + ',' + number_json(value.z) + ']';
}

std::uint64_t seconds_to_frames(const double seconds) {
    return static_cast<std::uint64_t>(std::max(0.0, seconds) * audio_output_rate + 0.5);
}

} // namespace

double reverb_zone_weight(const Scene& scene, const Entity zone_entity, const Vec3 point) {
    const auto* record = scene.get(zone_entity);
    if (!record || !record->reverb_zone) return 0.0;
    const auto& zone = *record->reverb_zone;
    const auto world = world_matrix(scene, zone_entity);
    if (!world) return 0.0;
    double outside = 0.0;
    if (zone.shape == ReverbZone::Shape::sphere) {
        double scale = 0.0;
        for (std::size_t axis = 0; axis < 3U; ++axis)
            scale = std::max(scale, length(matrix_column(*world, axis)));
        outside = length(subtract(point, matrix_column(*world, 3))) - zone.radius * scale;
    } else {
        // Clamp the point to the box in the node's space, then measure back in world space, so
        // rotation and scale are exact.
        const auto inverse = editor_inverse_affine(*world);
        if (!inverse) return 0.0;
        const auto local = transform_point(*inverse, point);
        const Vec3 clamped{std::clamp(local.x, -zone.half_extents.x, zone.half_extents.x),
                           std::clamp(local.y, -zone.half_extents.y, zone.half_extents.y),
                           std::clamp(local.z, -zone.half_extents.z, zone.half_extents.z)};
        outside = length(subtract(point, transform_point(*world, clamped)));
    }
    // Float transforms leave round-off of a few micrometres at the surface.
    if (outside <= 1e-4) return 1.0;
    if (zone.fade <= 0.0) return 0.0;
    return std::max(0.0, 1.0 - outside / zone.fade);
}

double audio_distance_gain(const AudioSource& source, const double distance) {
    const double minimum = std::max(source.min_distance, 1e-6);
    const double maximum = std::max(source.max_distance, minimum);
    if (!std::isfinite(distance) || distance >= maximum) return 0.0;
    if (distance <= minimum) return 1.0;
    double gain = 1.0;
    switch (source.rolloff) {
    case AudioSource::Rolloff::inverse: gain = minimum / distance; break;
    case AudioSource::Rolloff::inverse_square: gain = (minimum / distance) * (minimum / distance); break;
    case AudioSource::Rolloff::linear:
        return maximum > minimum ? 1.0 - (distance - minimum) / (maximum - minimum) : 0.0;
    }
    // The curved models never reach zero by themselves, so the last tenth of the range fades them
    // out; otherwise a sound would cut off abruptly when the listener crossed max_distance.
    const double fade_start = minimum + 0.9 * (maximum - minimum);
    if (distance > fade_start) gain *= (maximum - distance) / (maximum - fade_start);
    return gain;
}

struct AudioSystem::Device {
    ma_device device{};
    bool initialized{};
    ~Device() {
        if (initialized) ma_device_uninit(&device);
    }
};

AudioSystem::AudioSystem() = default;

AudioSystem::~AudioSystem() { close_output(); }

bool AudioSystem::open_output(std::string& error) {
    if (device_) return true;
    auto device = std::make_unique<Device>();
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = audio_output_rate;
    config.dataCallback = [](ma_device* raw, void* output, const void*, const ma_uint32 frames) {
        auto* system = static_cast<AudioSystem*>(raw->pUserData);
        system->render(std::span<float>(static_cast<float*>(output),
                                        static_cast<std::size_t>(frames) * 2U));
    };
    config.pUserData = this;
    if (ma_device_init(nullptr, &config, &device->device) != MA_SUCCESS) {
        error = output_error_ = "no audio playback device could be opened";
        return false;
    }
    device->initialized = true;
    if (ma_device_start(&device->device) != MA_SUCCESS) {
        error = output_error_ = "the audio playback device would not start";
        return false;
    }
    output_error_.clear();
    device_ = std::move(device);
    return true;
}

void AudioSystem::close_output() {
    // Stops the callback before anything it reads goes away.
    device_.reset();
}

AudioOutputStatus AudioSystem::output_status() const {
    AudioOutputStatus status;
    status.error = output_error_;
    if (!device_) return status;
    status.open = true;
    auto& device = device_->device;
    status.backend = ma_get_backend_name(device.pContext->backend);
    status.device = device.playback.name;
    status.sample_rate = device.sampleRate;
    return status;
}

void AudioSystem::set_project_root(std::optional<std::filesystem::path> root) {
    if (root == clips_.root()) return;
    stop_all();
    clips_.set_root(std::move(root));
}

void AudioSystem::set_settings(const AudioSettings& settings) {
    if (settings == base_settings_) return;
    base_settings_ = settings;
    apply_settings();
}

// The saved mixer with the game's script changes on top, pushed to the mixer when it differs.
void AudioSystem::apply_settings() {
    auto effective = base_settings_;
    for (auto& bus : effective.buses) {
        const auto found = bus_overrides_.find(bus.name);
        if (found == bus_overrides_.end()) continue;
        const auto& change = found->second;
        if (change.volume_db) bus.volume_db = *change.volume_db;
        if (change.mute) bus.mute = *change.mute;
        for (const auto& [key, value] : change.parameters)
            if (key.first < bus.effects.size())
                if (auto* target = audio_effect_parameter(bus.effects[key.first], key.second)) *target = value;
    }
    const std::lock_guard lock(mixer_mutex_);
    if (!(effective == mixer_.buses())) mixer_.set_buses(effective);
}

void AudioSystem::start_game() {
    stop_all();
    started_.clear();
    bus_overrides_.clear();
    apply_settings();
    game_running_ = true;
    paused_ = false;
    listener_last_position_.reset();
}

void AudioSystem::stop_game() {
    stop_all();
    started_.clear();
    bus_overrides_.clear();
    apply_settings();
    game_running_ = false;
    paused_ = false;
    zone_slots_ = {};
    listener_zones_.clear();
    const std::lock_guard lock(mixer_mutex_);
    mixer_.set_reverbs({});
}

void AudioSystem::set_paused(const bool paused) { paused_ = paused; }

void AudioSystem::find_listener(const Scene& scene, const double delta_seconds) {
    Listener listener;
    for (const auto entity : scene.entities()) {
        if (scene.get(entity)->audio_listener) {
            listener.entity = entity;
            break;
        }
    }
    if (!listener.entity) listener.entity = scene.active_camera();
    if (listener.entity) {
        if (const auto world = world_matrix(scene, *listener.entity)) {
            listener.position = matrix_column(*world, 3);
            listener.right = normalized(matrix_column(*world, 0), {1, 0, 0});
            listener.up = normalized(matrix_column(*world, 1), {0, 1, 0});
            listener.forward = scaled(normalized(matrix_column(*world, 2), {0, 0, 1}), -1.0);
        }
    }
    if (listener_last_position_ && delta_seconds > 0.0)
        listener.velocity =
            scaled(subtract(listener.position, *listener_last_position_), 1.0 / delta_seconds);
    if (delta_seconds > 0.0) listener_last_position_ = listener.position;
    listener_ = listener;
}

std::optional<Entity> AudioSystem::listener() const { return listener_.entity; }

std::optional<Vec3> AudioSystem::playback_position(const Scene& scene, const Playback& playback) const {
    if (playback.kind == Kind::one_shot) return playback.position;
    if (playback.kind == Kind::music) return std::nullopt;
    if (const auto world = world_matrix(scene, playback.entity)) return matrix_column(*world, 3);
    return std::nullopt;
}

AudioVoiceTarget AudioSystem::target_for(const Scene& scene, const AudioSource& source,
                                         Playback& playback, const double delta_seconds) const {
    AudioVoiceTarget target;
    target.bus = source.bus;
    target.loop = source.loop;
    target.paused = playback.game && paused_;
    target.rate = source.pitch * playback.pitch;
    const auto volume = audio_db_to_gain(
        std::clamp(source.volume_db + playback.volume_db, minimum_audio_volume_db, maximum_audio_volume_db));
    const auto position = playback_position(scene, playback);
    if (!playback.game || !source.spatial || !position) {
        // Balance: centre leaves both sides at full level.
        target.left = static_cast<float>(volume * std::min(1.0, 1.0 - source.pan));
        target.right = static_cast<float>(volume * std::min(1.0, 1.0 + source.pan));
        return target;
    }
    const auto offset = subtract(*position, listener_.position);
    const auto distance = length(offset);
    const auto distance_gain = volume * audio_distance_gain(source, distance);
    // Reverb: a sound excites the zones it is in; outside every zone, the ones the listener is in.
    const double send = distance_gain * source.reverb_send;
    std::array<double, maximum_reverb_slots> weights{};
    bool inside = false;
    for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot)
        if (zone_slots_[slot].valid()) {
            weights[slot] = reverb_zone_weight(scene, zone_slots_[slot], *position);
            inside |= weights[slot] > 0.0;
        }
    if (!inside)
        for (const auto& [zone, weight] : listener_zones_)
            for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot)
                if (zone_slots_[slot] == zone) weights[slot] = weight;
    for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot)
        target.sends[slot] = static_cast<float>(send * weights[slot]);
    // Occlusion muffles the direct sound only: the room still carries it.
    const auto gain = distance_gain * audio_db_to_gain(-audio_occlusion_db * playback.occlusion);
    double cutoff = 20000.0 * std::pow(audio_occlusion_cutoff_hz / 20000.0, playback.occlusion);
    if (base_settings_.spatialization == AudioSettings::Spatialization::binaural) {
        // Headphones: the head model places the sound; both ears get the distance gain.
        target.binaural = true;
        target.left = target.right = static_cast<float>(gain);
        if (distance > 1e-4) {
            const auto direction = scaled(offset, 1.0 / distance);
            const double across = dot(direction, listener_.right);
            const double ahead = dot(direction, listener_.forward);
            target.azimuth = static_cast<float>(std::atan2(across, ahead));
            target.elevation = static_cast<float>(std::asin(std::clamp(dot(direction, listener_.up), -1.0, 1.0)));
            if (ahead < 0.0)
                cutoff = std::min(cutoff, 20000.0 * std::pow(audio_behind_cutoff_hz / 20000.0, -ahead));
        }
    } else {
        // Equal-power pan from where the sound sits to the listener's right or left.
        const double side = distance > 1e-4 ? std::clamp(dot(offset, listener_.right) / distance, -1.0, 1.0)
                                            : 0.0;
        const double angle = (side + 1.0) * 0.5 * half_pi;
        target.left = static_cast<float>(gain * std::cos(angle));
        target.right = static_cast<float>(gain * std::sin(angle));
        target.downmix = true;
    }
    target.cutoff_hz = static_cast<float>(cutoff);
    if (source.doppler > 0.0 && playback.last_position && delta_seconds > 0.0 && distance > 1e-4) {
        const auto source_velocity =
            scaled(subtract(*position, *playback.last_position), 1.0 / delta_seconds);
        const auto toward_listener = scaled(offset, -1.0 / distance);
        const double listener_approach = -dot(listener_.velocity, toward_listener) * source.doppler;
        const double source_approach = dot(source_velocity, toward_listener) * source.doppler;
        const double ratio = (speed_of_sound + listener_approach) /
                             std::max(1.0, speed_of_sound - source_approach);
        target.rate *= std::clamp(ratio, 0.5, 2.0);
    }
    if (delta_seconds > 0.0) playback.last_position = *position;
    return target;
}

std::uint64_t AudioSystem::start(Playback playback, const AudioVoiceTarget& target,
                                 std::string& error, const std::uint64_t delay_frames,
                                 const float fade) {
    // Decoding happens before the lock, so a long file never stalls the device thread.
    const bool stream = clips_.streams(playback.clip, error);
    if (!error.empty()) return 0;
    std::shared_ptr<const AudioClip> clip;
    std::shared_ptr<AudioStream> opened;
    std::size_t decoded = 0;
    bool finished = false;
    if (stream) {
        const auto resolved = clips_.resolve(playback.clip, error);
        if (!resolved) return 0;
        opened = AudioStream::open(*resolved, target.loop, error);
        if (!opened) {
            error = playback.clip + ": " + error;
            return 0;
        }
        decoded = opened->decode(scratch_, 0, finished);
    } else {
        clip = clips_.load(playback.clip, error);
        if (!clip) return 0;
    }
    const std::lock_guard lock(mixer_mutex_);
    if (opened) opened->commit(scratch_, decoded, finished);
    const auto voice = opened ? mixer_.play(opened, target, delay_frames)
                              : mixer_.play(clip, target, 0.0, delay_frames);
    if (voice == 0U) {
        error = "every one of the " + std::to_string(maximum_audio_voices) +
                " voices is already playing";
        return 0;
    }
    if (fade != 1.0F) (void)mixer_.set_fade(voice, fade);
    playback.voice = voice;
    playback.stream = std::move(opened);
    const auto handle = next_handle_++;
    playbacks_.emplace(handle, std::move(playback));
    return handle;
}

bool AudioSystem::start_source(const Scene& scene, const Entity entity, const bool game,
                               std::string& error, const double volume_db, const double pitch) {
    const auto* record = scene.get(entity);
    if (!record || !record->audio_source) {
        error = "the node has no audio source";
        return false;
    }
    const auto& source = *record->audio_source;
    if (source.clip.empty()) {
        error = "the audio source has no clip";
        return false;
    }
    (void)stop(entity);
    Playback playback;
    playback.kind = Kind::source;
    playback.entity = entity;
    playback.clip = source.clip;
    playback.game = game;
    playback.volume_db = std::isfinite(volume_db) ? volume_db : 0.0;
    playback.pitch = std::isfinite(pitch) ? std::clamp(pitch, 0.1, 4.0) : 1.0;
    const auto target = target_for(scene, source, playback, 0.0);
    const auto handle = start(std::move(playback), target, error);
    if (handle == 0U) return false;
    sources_[entity] = handle;
    return true;
}

bool AudioSystem::play(const Scene& scene, const Entity entity, const bool game,
                       std::string& error, const double volume_db, const double pitch) {
    return start_source(scene, entity, game && game_running_, error, volume_db, pitch);
}

bool AudioSystem::stop(const Entity entity) {
    const auto found = sources_.find(entity);
    if (found == sources_.end()) return false;
    const auto handle = found->second;
    sources_.erase(found);
    return stop_sound(handle);
}

void AudioSystem::stop_all() {
    const std::lock_guard lock(mixer_mutex_);
    mixer_.stop_all();
    playbacks_.clear();
    sources_.clear();
    music_.clear();
}

bool AudioSystem::playing(const Entity entity) const {
    const auto found = sources_.find(entity);
    return found != sources_.end() && sound_playing(found->second);
}

std::optional<double> AudioSystem::position(const Entity entity) const {
    const auto found = sources_.find(entity);
    if (found == sources_.end()) return std::nullopt;
    return sound_position(found->second);
}

std::uint64_t AudioSystem::play_clip(const Scene& scene, const std::string_view clip,
                                     const AudioOneShot& options, std::string& error) {
    if (!valid_audio_clip_path(clip)) {
        error = "one-shot clips must be project-relative .wav, .flac, .mp3 or .ogg files";
        return 0;
    }
    AudioSource settings;
    settings.clip = std::string(clip);
    settings.bus = options.bus;
    settings.volume_db = 0.0;
    settings.spatial = options.position.has_value();
    settings.min_distance = options.min_distance;
    settings.max_distance = options.max_distance;
    settings.play_on_start = false;
    settings.doppler = 0.0;
    if (!valid_audio_source(settings) || !std::isfinite(options.volume_db) || !std::isfinite(options.pitch)) {
        error = "one-shot bus, volume, pitch or distances out of range";
        return 0;
    }
    Playback playback;
    playback.kind = Kind::one_shot;
    playback.clip = settings.clip;
    playback.game = game_running_;
    playback.volume_db = std::clamp(options.volume_db, minimum_audio_volume_db, maximum_audio_volume_db);
    playback.pitch = std::clamp(options.pitch, 0.1, 4.0);
    playback.settings = settings;
    playback.position = options.position;
    const auto target = target_for(scene, settings, playback, 0.0);
    return start(std::move(playback), target, error);
}

bool AudioSystem::stop_sound(const std::uint64_t handle) {
    const std::lock_guard lock(mixer_mutex_);
    const auto found = playbacks_.find(handle);
    if (found == playbacks_.end()) return false;
    const bool was_playing = mixer_.active(found->second.voice);
    mixer_.stop(found->second.voice);
    playbacks_.erase(found);
    return was_playing;
}

bool AudioSystem::sound_playing(const std::uint64_t handle) const {
    const std::lock_guard lock(mixer_mutex_);
    const auto found = playbacks_.find(handle);
    return found != playbacks_.end() && mixer_.active(found->second.voice);
}

std::optional<double> AudioSystem::sound_position(const std::uint64_t handle) const {
    const std::lock_guard lock(mixer_mutex_);
    const auto found = playbacks_.find(handle);
    if (found == playbacks_.end()) return std::nullopt;
    return mixer_.position_seconds(found->second.voice);
}

AudioVoiceTarget AudioSystem::music_target(const MusicPlayer& player) const {
    AudioVoiceTarget target;
    target.bus = player.bus;
    target.left = target.right = static_cast<float>(audio_db_to_gain(player.volume_db));
    // A one-track playlist that loops is one looping voice, with no gap at the join.
    target.loop = player.tracks.size() == 1U && player.loop_playlist;
    target.paused = paused_;
    return target;
}

int AudioSystem::next_track(const MusicPlayer& player, MusicState& state, bool& wrapped) const {
    wrapped = false;
    const auto count = static_cast<int>(player.tracks.size());
    if (count == 0) return -1;
    const auto shuffle = [&] {
        state.order.resize(static_cast<std::size_t>(count));
        std::iota(state.order.begin(), state.order.end(), 0);
        if (!player.shuffle) return;
        // A repeatable shuffle, different each time round the playlist.
        std::uint64_t seed = 0x9E3779B97F4A7C15ULL * (state.plays + 1U) ^ 0xD1B54A32D192ED03ULL;
        for (int index = count - 1; index > 0; --index) {
            seed ^= seed >> 33U;
            seed *= 0xFF51AFD7ED558CCDULL;
            seed ^= seed >> 33U;
            std::swap(state.order[static_cast<std::size_t>(index)],
                      state.order[static_cast<std::size_t>(seed % static_cast<std::uint64_t>(index + 1))]);
        }
    };
    if (state.order.size() != static_cast<std::size_t>(count) || state.track < 0) {
        shuffle();
        state.order_index = 0;
        return state.order.front();
    }
    auto index = state.order_index + 1U;
    if (index >= state.order.size()) {
        if (!player.loop_playlist) return -1;
        shuffle();
        index = 0;
        wrapped = true;
    }
    state.order_index = index;
    return state.order[index];
}

bool AudioSystem::change_track(const Scene& scene, const Entity entity, int track,
                               const MusicPlayer::Sync sync, std::string& error) {
    const auto* record = scene.get(entity);
    if (!record || !record->music_player) {
        error = "the node has no music player";
        return false;
    }
    const auto& player = *record->music_player;
    if (player.tracks.empty()) {
        error = "the music player has no tracks";
        return false;
    }
    auto& state = music_[entity];
    if (track < 0) {
        bool wrapped = false;
        track = next_track(player, state, wrapped);
        if (track < 0) {
            error = "the playlist has ended";
            return false;
        }
    } else if (track >= static_cast<int>(player.tracks.size())) {
        error = "the playlist has no track " + std::to_string(track);
        return false;
    } else {
        bool wrapped = false;
        if (state.order.size() != player.tracks.size()) (void)next_track(player, state, wrapped);
        const auto at = std::find(state.order.begin(), state.order.end(), track);
        state.order_index = static_cast<std::size_t>(at - state.order.begin());
    }
    // When the change lands: now, or on the current track's next beat, bar or ending.
    std::uint64_t delay = 0;
    std::uint64_t old_voice = 0;
    const auto current = playbacks_.find(state.current);
    if (current != playbacks_.end()) {
        old_voice = current->second.voice;
        std::optional<AudioVoiceInfo> info;
        {
            const std::lock_guard lock(mixer_mutex_);
            info = mixer_.voice(old_voice);
        }
        if (info && sync != MusicPlayer::Sync::immediate) {
            const double position = info->position_seconds;
            double boundary = position;
            if (sync == MusicPlayer::Sync::track_end) {
                if (info->duration_seconds > 0.0)
                    boundary = std::max(position, info->duration_seconds - player.crossfade_seconds);
            } else {
                const double period = 60.0 / player.bpm *
                                      (sync == MusicPlayer::Sync::bar ? player.beats_per_bar : 1U);
                const double since = position - player.first_beat_seconds;
                boundary = since < 0.0 ? player.first_beat_seconds
                                       : player.first_beat_seconds +
                                             std::ceil(since / period - 1e-9) * period;
            }
            delay = seconds_to_frames(boundary - position);
        }
    }
    const auto crossfade = std::max<std::uint64_t>(seconds_to_frames(player.crossfade_seconds), 1U);
    Playback playback;
    playback.kind = Kind::music;
    playback.entity = entity;
    playback.clip = player.tracks[static_cast<std::size_t>(track)];
    playback.game = true;
    playback.track = track;
    // With nothing playing the track starts at full volume; otherwise it fades in as the old one
    // fades out. Its fade starts with it, since the voice's own delay already holds it back.
    const auto handle = start(std::move(playback), music_target(player), error, delay,
                              old_voice ? 0.0F : 1.0F);
    if (handle == 0U) return false;
    {
        const std::lock_guard lock(mixer_mutex_);
        if (old_voice) (void)mixer_.fade(playbacks_.at(handle).voice, 1.0F, 0, crossfade, false);
        if (old_voice) {
            (void)mixer_.fade(old_voice, 0.0F, delay, crossfade, true);
            current->second.fading_out = true;
        }
    }
    state.current = handle;
    state.track = track;
    state.advance_scheduled = false;
    state.stopped = false;
    state.started = true;
    ++state.plays;
    return true;
}

bool AudioSystem::music_play(const Scene& scene, const Entity entity, const int track,
                             const std::optional<MusicPlayer::Sync> sync, std::string& error) {
    if (!game_running_) {
        error = "music players play during the game";
        return false;
    }
    const auto* record = scene.get(entity);
    if (!record || !record->music_player) {
        error = "the node has no music player";
        return false;
    }
    return change_track(scene, entity, track, sync.value_or(record->music_player->sync), error);
}

bool AudioSystem::music_stop(const Entity entity, const double fade_seconds) {
    const auto found = music_.find(entity);
    if (found == music_.end()) return false;
    auto& state = found->second;
    state.stopped = true;
    const auto current = playbacks_.find(state.current);
    state.current = 0;
    state.track = -1;
    if (current == playbacks_.end()) return false;
    const std::lock_guard lock(mixer_mutex_);
    (void)mixer_.fade(current->second.voice, 0.0F, 0,
                      std::max<std::uint64_t>(seconds_to_frames(fade_seconds), 1U), true);
    current->second.fading_out = true;
    return true;
}

int AudioSystem::music_track(const Entity entity) const {
    const auto found = music_.find(entity);
    if (found == music_.end() || !playbacks_.contains(found->second.current)) return -1;
    return found->second.track;
}

void AudioSystem::update_music(const Scene& scene, const double delta_seconds) {
    (void)delta_seconds;
    for (auto item = music_.begin(); item != music_.end();) {
        const auto* record = scene.get(item->first);
        if (record && record->music_player) {
            ++item;
            continue;
        }
        // The player went away: so does its music.
        const std::lock_guard lock(mixer_mutex_);
        for (auto& [handle, playback] : playbacks_)
            if (playback.kind == Kind::music && playback.entity == item->first) mixer_.stop(playback.voice);
        item = music_.erase(item);
    }
    for (const auto entity : scene.entities()) {
        const auto& player = scene.get(entity)->music_player;
        if (!player || player->tracks.empty()) continue;
        auto& state = music_[entity];
        std::string ignored;
        if (!state.started) {
            state.started = true;
            if (player->play_on_start)
                (void)change_track(scene, entity, -1, MusicPlayer::Sync::immediate, ignored);
            continue;
        }
        if (state.stopped || state.current == 0U) continue;
        const auto current = playbacks_.find(state.current);
        if (current == playbacks_.end()) {
            // The track ended without a follower: its length was unknown, so go on now, or the
            // playlist is over.
            state.current = 0;
            state.track = -1;
            if (!change_track(scene, entity, -1, MusicPlayer::Sync::immediate, ignored))
                state.stopped = true;
            continue;
        }
        if (state.advance_scheduled) continue;
        std::optional<AudioVoiceInfo> info;
        {
            const std::lock_guard lock(mixer_mutex_);
            info = mixer_.voice(current->second.voice);
        }
        if (!info || info->loop || info->duration_seconds <= 0.0) continue;
        // Schedule the next track shortly before its crossfade is due, to the exact frame.
        const double remaining = info->duration_seconds - info->position_seconds;
        if (remaining - player->crossfade_seconds > 0.25) continue;
        // With nothing to follow (the playlist is over), the flag stays and the track ends.
        state.advance_scheduled = true;
        (void)change_track(scene, entity, -1, MusicPlayer::Sync::track_end, ignored);
    }
}

bool AudioSystem::set_bus_volume(const std::string_view bus, const double volume_db,
                                 const double fade_seconds, std::string& error) {
    if (!game_running_) {
        error = "scripts change buses during the game; the Mixer changes them for good";
        return false;
    }
    const auto current = bus_volume(bus);
    if (!current) {
        error = "no bus is named " + std::string(bus);
        return false;
    }
    if (!std::isfinite(volume_db) || !std::isfinite(fade_seconds)) {
        error = "bus volume and fade must be numbers";
        return false;
    }
    auto& change = bus_overrides_[std::string(bus)];
    change.fade_from = *current;
    change.fade_to = std::clamp(volume_db, minimum_audio_volume_db, maximum_audio_volume_db);
    change.fade_elapsed = 0.0;
    change.fade_duration = std::max(fade_seconds, 0.0);
    change.volume_db = change.fade_duration > 0.0 ? change.fade_from : change.fade_to;
    apply_settings();
    return true;
}

bool AudioSystem::set_bus_mute(const std::string_view bus, const bool mute, std::string& error) {
    if (!game_running_) {
        error = "scripts change buses during the game; the Mixer changes them for good";
        return false;
    }
    if (!bus_volume(bus)) {
        error = "no bus is named " + std::string(bus);
        return false;
    }
    bus_overrides_[std::string(bus)].mute = mute;
    apply_settings();
    return true;
}

bool AudioSystem::set_bus_effect(const std::string_view bus, const std::size_t index,
                                 const std::string_view parameter, const double value,
                                 std::string& error) {
    if (!game_running_) {
        error = "scripts change buses during the game; the Mixer changes them for good";
        return false;
    }
    const auto found = std::find_if(base_settings_.buses.begin(), base_settings_.buses.end(),
                                    [&](const AudioBus& item) { return item.name == bus; });
    if (found == base_settings_.buses.end() || index >= found->effects.size()) {
        error = "bus " + std::string(bus) + " has no effect " + std::to_string(index);
        return false;
    }
    auto effect = found->effects[index];
    const auto parameters = audio_effect_parameters(effect.type);
    auto* target = audio_effect_parameter(effect, parameter);
    if (!target || std::find(parameters.begin(), parameters.end(), parameter) == parameters.end()) {
        error = std::string(audio_effect_type_name(effect.type)) + " has no parameter " + std::string(parameter);
        return false;
    }
    *target = value;
    if (!valid_audio_effect(effect, error)) return false;
    bus_overrides_[std::string(bus)].parameters[{index, std::string(parameter)}] = value;
    apply_settings();
    return true;
}

std::optional<double> AudioSystem::bus_volume(const std::string_view bus) const {
    for (const auto& item : base_settings_.buses) {
        if (item.name != bus) continue;
        const auto change = bus_overrides_.find(bus);
        return change != bus_overrides_.end() && change->second.volume_db ? *change->second.volume_db
                                                                             : item.volume_db;
    }
    return std::nullopt;
}

void AudioSystem::update_zones(const Scene& scene, const bool game) {
    listener_zones_.clear();
    std::array<std::optional<AudioReverbParameters>, maximum_reverb_slots> reverbs{};
    if (!(game && game_running_)) {
        zone_slots_ = {};
        const std::lock_guard lock(mixer_mutex_);
        mixer_.set_reverbs(reverbs);
        return;
    }
    // A zone matters while the listener or a playing, placed sound is in or near it.
    std::vector<Vec3> sounds;
    for (const auto& [handle, playback] : playbacks_)
        if (playback.game && playback.kind != Kind::music)
            if (const auto position = playback_position(scene, playback)) sounds.push_back(*position);
    std::vector<Entity> relevant;
    for (const auto entity : scene.entities()) {
        if (!scene.get(entity)->reverb_zone) continue;
        const double heard = reverb_zone_weight(scene, entity, listener_.position);
        if (heard > 0.0) listener_zones_.emplace_back(entity, heard);
        const bool near = heard > 0.0 || std::any_of(sounds.begin(), sounds.end(), [&](const Vec3 point) {
                              return reverb_zone_weight(scene, entity, point) > 0.0;
                          });
        if (near) relevant.push_back(entity);
    }
    // Slots stay with their zones while they matter, so each reverb's tail is its own.
    for (auto& zone : zone_slots_)
        if (zone.valid() && std::find(relevant.begin(), relevant.end(), zone) == relevant.end()) zone = {};
    for (const auto zone : relevant) {
        if (std::find(zone_slots_.begin(), zone_slots_.end(), zone) != zone_slots_.end()) continue;
        const auto free = std::find(zone_slots_.begin(), zone_slots_.end(), Entity{});
        if (free == zone_slots_.end()) break; // More zones at once than slots: the rest are dry.
        *free = zone;
    }
    for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot) {
        if (!zone_slots_[slot].valid()) continue;
        const auto& zone = *scene.get(zone_slots_[slot])->reverb_zone;
        reverbs[slot] = AudioReverbParameters{zone.room_size, zone.damping, zone.wet_db, zone.pre_delay_ms};
    }
    const std::lock_guard lock(mixer_mutex_);
    mixer_.set_reverbs(reverbs);
}

void AudioSystem::update_occlusion(const Scene& scene, const double delta_seconds,
                                   const AudioRaycast& raycast) {
    // Rays from the listener to each positioned sound, taking turns when there are more than the
    // per-step budget. The ray skips the body the listener rides in, such as the player's capsule
    // around its camera.
    Entity ignore{};
    for (auto current = *listener_.entity; current.valid();) {
        const auto* record = scene.get(current);
        if (!record) break;
        if (record->collider) {
            ignore = current;
            break;
        }
        current = record->parent;
    }
    std::vector<Playback*> candidates;
    for (auto& [handle, playback] : playbacks_) {
        const AudioSource* source = playback.kind == Kind::one_shot ? &playback.settings : nullptr;
        if (playback.kind == Kind::source)
            if (const auto* record = scene.get(playback.entity); record && record->audio_source)
                source = &*record->audio_source;
        if (!playback.game || !source || !source->spatial || !source->occlusion) {
            playback.occlusion_target = 0.0;
            continue;
        }
        candidates.push_back(&playback);
    }
    const auto rays = std::min(candidates.size(), audio_occlusion_rays);
    for (std::size_t ray = 0; ray < rays; ++ray) {
        auto* playback = candidates[(occlusion_cursor_ + ray) % candidates.size()];
        const auto position = playback_position(scene, *playback);
        if (!position) continue;
        const auto offset = subtract(*position, listener_.position);
        const auto distance = length(offset);
        if (distance < 0.05) {
            playback->occlusion_target = 0.0;
            continue;
        }
        const auto hit = raycast(listener_.position, scaled(offset, 1.0 / distance), distance - 0.02, ignore);
        playback->occlusion_target =
            hit && !(playback->kind == Kind::source && related(scene, *hit, playback->entity)) ? 1.0 : 0.0;
    }
    if (!candidates.empty()) occlusion_cursor_ = (occlusion_cursor_ + rays) % candidates.size();
    const double step = delta_seconds / audio_occlusion_seconds;
    for (auto& [handle, playback] : playbacks_)
        playback.occlusion += std::clamp(playback.occlusion_target - playback.occlusion, -step, step);
}

void AudioSystem::refill_streams() {
    for (auto& [handle, playback] : playbacks_) {
        if (!playback.stream) continue;
        std::uint64_t consumed = 0;
        {
            const std::lock_guard lock(mixer_mutex_);
            consumed = playback.stream->consumed;
        }
        bool finished = false;
        const auto decoded = playback.stream->decode(scratch_, consumed, finished);
        if (decoded == 0U && !finished) continue;
        const std::lock_guard lock(mixer_mutex_);
        playback.stream->commit(scratch_, decoded, finished);
    }
}

void AudioSystem::update(const Scene& scene, const bool game, const double delta_seconds,
                         const AudioRaycast& raycast) {
    find_listener(scene, delta_seconds);
    const bool running = game && game_running_;
    // Script bus fades move with game time.
    if (running && !paused_ && delta_seconds > 0.0) {
        bool changed = false;
        for (auto& [name, change] : bus_overrides_) {
            if (change.fade_duration <= 0.0 || change.fade_elapsed >= change.fade_duration) continue;
            change.fade_elapsed = std::min(change.fade_duration, change.fade_elapsed + delta_seconds);
            change.volume_db = change.fade_from + (change.fade_to - change.fade_from) *
                                                      (change.fade_elapsed / change.fade_duration);
            changed = true;
        }
        if (changed) apply_settings();
    }
    update_zones(scene, game);
    if (running && raycast && listener_.entity && delta_seconds > 0.0)
        update_occlusion(scene, delta_seconds, raycast);
    if (running && !paused_) {
        for (const auto entity : scene.entities()) {
            const auto& source = scene.get(entity)->audio_source;
            if (!source || !source->play_on_start || started_.contains(entity)) continue;
            started_.insert(entity);
            std::string ignored;
            if (!source->clip.empty()) (void)start_source(scene, entity, true, ignored);
        }
        update_music(scene, delta_seconds);
    }
    {
        const std::lock_guard lock(mixer_mutex_);
        for (auto item = playbacks_.begin(); item != playbacks_.end();) {
            auto& [handle, playback] = *item;
            const auto* record = playback.kind == Kind::one_shot ? nullptr : scene.get(playback.entity);
            bool keep = mixer_.active(playback.voice) && (!playback.game || game);
            const AudioSource* source = nullptr;
            if (playback.kind == Kind::source) {
                keep = keep && record && record->audio_source && record->audio_source->clip == playback.clip;
                if (keep) source = &*record->audio_source;
            } else if (playback.kind == Kind::one_shot) {
                source = &playback.settings;
            } else {
                keep = keep && record && record->music_player;
            }
            if (!keep) {
                mixer_.stop(playback.voice);
                if (playback.kind == Kind::source) {
                    const auto owner = sources_.find(playback.entity);
                    if (owner != sources_.end() && owner->second == handle) sources_.erase(owner);
                }
                item = playbacks_.erase(item);
                continue;
            }
            if (source)
                (void)mixer_.update(playback.voice, target_for(scene, *source, playback, delta_seconds));
            else
                (void)mixer_.update(playback.voice, music_target(*record->music_player));
            ++item;
        }
    }
    refill_streams();
    // Without a device nothing else pulls samples, so the simulation's own clock does.
    if (!device_ && delta_seconds > 0.0) {
        pending_frames_ += delta_seconds * audio_output_rate;
        const auto frames = static_cast<std::size_t>(pending_frames_);
        pending_frames_ -= static_cast<double>(frames);
        // Streams are refilled between slices, as a device would pull them between game steps.
        constexpr std::size_t slice = 4096U;
        std::vector<float> discarded(std::min(frames, slice) * 2U);
        for (std::size_t done = 0; done < frames; done += slice) {
            const auto count = std::min(slice, frames - done);
            render(std::span<float>(discarded.data(), count * 2U));
            refill_streams();
        }
    }
}

void AudioSystem::render(const std::span<float> output) {
    const std::lock_guard lock(mixer_mutex_);
    mixer_.render(output);
}

std::uint64_t AudioSystem::rendered_frames() const {
    const std::lock_guard lock(mixer_mutex_);
    return mixer_.rendered_frames();
}

std::string AudioSystem::status_json(const Scene& scene) const {
    const auto output = output_status();
    std::string json = "{\"output\":{\"open\":" + std::string(output.open ? "true" : "false") +
                       ",\"backend\":\"" + json_escape(output.backend) + "\",\"device\":\"" +
                       json_escape(output.device) +
                       "\",\"sample_rate\":" + std::to_string(output.sample_rate) +
                       ",\"error\":\"" + json_escape(output.error) + "\"}";
    json += ",\"sample_rate\":" + std::to_string(audio_output_rate);
    json += ",\"spatialization\":\"" +
            std::string(base_settings_.spatialization == AudioSettings::Spatialization::binaural ? "binaural"
                                                                                                  : "stereo") +
            '"';
    json += ",\"game\":" + std::string(game_running_ ? "true" : "false");
    json += ",\"paused\":" + std::string(paused_ ? "true" : "false");
    json += ",\"listener\":";
    json += listener_.entity ? '"' + listener_.entity->to_string() + '"' : std::string("null");
    const std::lock_guard lock(mixer_mutex_);
    json += ",\"rendered_frames\":" + std::to_string(mixer_.rendered_frames());
    json += ",\"voices\":[";
    bool first = true;
    for (const auto& [handle, playback] : playbacks_) {
        const auto info = mixer_.voice(playback.voice);
        if (!info) continue;
        const bool node = playback.kind != Kind::one_shot;
        const auto* record = node ? scene.get(playback.entity) : nullptr;
        if (!first) json += ',';
        first = false;
        json += "{\"handle\":" + std::to_string(handle) + ",\"kind\":\"" +
                (playback.kind == Kind::source ? "source" : playback.kind == Kind::music ? "music" : "one_shot") +
                "\",\"entity\":" + (node ? '"' + playback.entity.to_string() + '"' : std::string("null")) +
                ",\"name\":\"" + json_escape(record ? record->name : std::string{}) + "\",\"clip\":\"" +
                json_escape(playback.clip) + "\",\"bus\":\"" + json_escape(info->bus) +
                "\",\"position_seconds\":" + number_json(info->position_seconds) +
                ",\"duration_seconds\":" + number_json(info->duration_seconds) +
                ",\"gain_left\":" + number_json(info->left) +
                ",\"gain_right\":" + number_json(info->right) +
                ",\"send\":" + number_json(info->send) +
                ",\"cutoff_hz\":" + number_json(info->cutoff_hz) +
                ",\"occlusion\":" + number_json(playback.occlusion) +
                ",\"fade\":" + number_json(info->fade) +
                ",\"track\":" + std::to_string(playback.track) +
                ",\"loop\":" + (info->loop ? "true" : "false") +
                ",\"paused\":" + (info->paused ? "true" : "false") +
                ",\"streaming\":" + (info->streaming ? "true" : "false") +
                ",\"waiting\":" + (info->waiting ? "true" : "false") +
                ",\"preview\":" + (playback.game ? "false" : "true") + '}';
    }
    json += "],\"reverb\":{\"zones\":[";
    first = true;
    const auto& reverbs = mixer_.reverbs();
    for (std::size_t slot = 0; slot < maximum_reverb_slots; ++slot) {
        if (!zone_slots_[slot].valid() || !reverbs[slot]) continue;
        double heard = 0.0;
        for (const auto& [zone, weight] : listener_zones_)
            if (zone == zone_slots_[slot]) heard = weight;
        const auto& parameters = *reverbs[slot];
        if (!first) json += ',';
        first = false;
        json += "{\"entity\":\"" + zone_slots_[slot].to_string() + "\",\"slot\":" + std::to_string(slot) +
                ",\"listener_weight\":" + number_json(heard) +
                ",\"room_size\":" + number_json(parameters.room_size) +
                ",\"damping\":" + number_json(parameters.damping) +
                ",\"wet_db\":" + number_json(parameters.wet_db) +
                ",\"pre_delay_ms\":" + number_json(parameters.pre_delay_ms) + '}';
    }
    json += "]},\"music\":[";
    first = true;
    for (const auto& [entity, state] : music_) {
        if (!first) json += ',';
        first = false;
        const bool playing = playbacks_.contains(state.current);
        json += "{\"entity\":\"" + entity.to_string() + "\",\"track\":" +
                std::to_string(playing ? state.track : -1) + ",\"playing\":" + (playing ? "true" : "false") + '}';
    }
    json += "],\"buses\":[";
    const auto& buses = mixer_.buses().buses;
    const auto levels = mixer_.levels();
    for (std::size_t index = 0; index < buses.size(); ++index) {
        const auto& bus = buses[index];
        const auto& level = levels[index];
        if (index) json += ',';
        json += "{\"name\":\"" + json_escape(bus.name) + "\",\"parent\":" +
                (bus.parent.empty() ? std::string("null") : '"' + json_escape(bus.parent) + '"') +
                ",\"volume_db\":" + number_json(bus.volume_db) +
                ",\"mute\":" + (bus.mute ? "true" : "false") +
                ",\"solo\":" + (bus.solo ? "true" : "false") +
                ",\"peak_left_db\":" + number_json(level.peak_left_db) +
                ",\"peak_right_db\":" + number_json(level.peak_right_db) +
                ",\"rms_db\":" + number_json(level.rms_db) + '}';
    }
    return json + "]}";
}

std::string AudioSystem::debug_shapes_json(const Scene& scene) {
    std::string zones, sources;
    for (const auto entity : scene.entities()) {
        const auto* record = scene.get(entity);
        if (!record->reverb_zone && !(record->audio_source && record->audio_source->spatial)) continue;
        const auto world = world_matrix(scene, entity);
        if (!world) continue;
        const auto center = matrix_column(*world, 3);
        if (const auto& zone = record->reverb_zone) {
            if (!zones.empty()) zones += ',';
            zones += "{\"entity\":\"" + entity.to_string() + "\",\"shape\":\"" +
                     std::string(reverb_shape_name(zone->shape)) + "\",\"center\":";
            append_vec3(zones, center);
            double scale = 0.0;
            zones += ",\"edges\":[";
            const std::array<double, 3> half{zone->half_extents.x, zone->half_extents.y,
                                             zone->half_extents.z};
            for (std::size_t axis = 0; axis < 3U; ++axis) {
                const auto column = matrix_column(*world, axis);
                scale = std::max(scale, length(column));
                if (axis) zones += ',';
                append_vec3(zones, scaled(column, half[axis]));
            }
            zones += "],\"radius\":" + number_json(zone->radius * scale) +
                     ",\"fade\":" + number_json(zone->fade) + '}';
        }
        if (const auto& source = record->audio_source; source && source->spatial) {
            if (!sources.empty()) sources += ',';
            sources += "{\"entity\":\"" + entity.to_string() + "\",\"center\":";
            append_vec3(sources, center);
            sources += ",\"min_distance\":" + number_json(source->min_distance) +
                       ",\"max_distance\":" + number_json(source->max_distance) + '}';
        }
    }
    return "{\"zones\":[" + zones + "],\"sources\":[" + sources + "]}";
}

std::optional<std::string> AudioSystem::clip_json(const std::string_view path,
                                                  const std::size_t peaks, std::string& error) {
    const bool stream = clips_.streams(path, error);
    if (!error.empty()) return std::nullopt;
    std::uint32_t sample_rate = 0, channels = 0;
    std::uint64_t frames = 0;
    std::vector<float> values;
    if (stream) {
        const auto resolved = clips_.resolve(path, error);
        if (!resolved) return std::nullopt;
        auto summary = summarize_audio_file(*resolved, peaks, error);
        if (!summary) return std::nullopt;
        sample_rate = summary->sample_rate;
        channels = summary->channels;
        frames = summary->frames;
        values = std::move(summary->peaks);
    } else {
        const auto clip = clips_.load(path, error);
        if (!clip) return std::nullopt;
        sample_rate = clip->sample_rate;
        channels = clip->channels;
        frames = clip->frames();
        const auto buckets = std::min<std::uint64_t>(peaks, frames);
        for (std::uint64_t bucket = 0; bucket < buckets; ++bucket) {
            const auto begin = bucket * frames / buckets;
            const auto end = std::max(begin + 1U, (bucket + 1U) * frames / buckets);
            float low = 0.0F, high = 0.0F;
            for (auto frame = begin; frame < end; ++frame) {
                float value = 0.0F;
                for (std::uint32_t channel = 0; channel < channels; ++channel)
                    value += clip->samples[frame * channels + channel];
                value /= static_cast<float>(channels);
                low = std::min(low, value);
                high = std::max(high, value);
            }
            values.push_back(low);
            values.push_back(high);
        }
    }
    std::string json = "{\"clip\":\"" + json_escape(path) + "\",\"duration_seconds\":" +
                       number_json(sample_rate ? static_cast<double>(frames) / sample_rate : 0.0) +
                       ",\"sample_rate\":" + std::to_string(sample_rate) +
                       ",\"channels\":" + std::to_string(channels) +
                       ",\"frames\":" + std::to_string(frames) +
                       ",\"streams\":" + (stream ? "true" : "false") + ",\"peaks\":[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) json += ',';
        json += number_json(std::round(values[index] * 1000.0F) / 1000.0F);
    }
    return json + "]}";
}

} // namespace relay
