#pragma once

#include "relay/audio/audio_clip.hpp"
#include "relay/audio/audio_mixer.hpp"
#include "relay/audio/audio_settings.hpp"
#include "relay/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace relay {

struct AudioOutputStatus {
    bool open{};
    std::string backend;
    std::string device;
    std::uint32_t sample_rate{};
    std::string error; // Why the output is closed, if it was asked for.
};

// Distance attenuation for a source `distance` metres from the listener.
[[nodiscard]] double audio_distance_gain(const AudioSource& source, double distance);
// How strongly the reverb zone on `zone` applies at a world point: 1 inside its shape, falling
// linearly to 0 at `fade` metres outside it. 0 without a zone.
[[nodiscard]] double reverb_zone_weight(const Scene& scene, Entity zone, Vec3 point);

// The first collider a ray hits within `distance`, skipping `ignore`, for occlusion. Supplied by
// the engine from the running physics world.
using AudioRaycast =
    std::function<std::optional<Entity>(Vec3 origin, Vec3 direction, double distance, Entity ignore)>;

// Occluded sounds lose this much level and fall to this low-pass cutoff.
inline constexpr double audio_occlusion_db = 12.0;
inline constexpr double audio_occlusion_cutoff_hz = 800.0;
// Occlusion changes over about this time, so walking past a pillar does not click.
inline constexpr double audio_occlusion_seconds = 0.15;
// Rays per game step; with more occluding sources they take turns.
inline constexpr std::size_t audio_occlusion_rays = 64U;
// Binaural: a sound straight behind the listener is low-passed to this, a cue for front and back.
inline constexpr double audio_behind_cutoff_hz = 6000.0;

// A sound played from a file rather than a node's audio source. With a position it is placed
// in the world during the game (flat in the editor); without one it plays flat.
struct AudioOneShot {
    std::optional<Vec3> position;
    double volume_db{};
    double pitch{1.0};
    std::string bus{"SFX"};
    double min_distance{1.0};
    double max_distance{50.0};
};

// Plays the scene's audio through the mixer: node audio sources, one-shots, and music players.
// The game thread updates voices once per game step; the mixer is rendered either by a sound
// device on its own thread or, without one, by the game steps themselves, so headless runs advance
// audio deterministically with the simulation.
class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Opens the default playback device. Tests and headless sessions never call it.
    bool open_output(std::string& error);
    void close_output();
    [[nodiscard]] AudioOutputStatus output_status() const;

    void set_project_root(std::optional<std::filesystem::path> root);
    // The saved (or previewed) mixer; game-time bus changes from scripts apply on top of it.
    void set_settings(const AudioSettings& settings);

    // Game lifecycle: sources with play_on_start begin on the first update after start.
    void start_game();
    void stop_game();
    void set_paused(bool paused);
    // Spatialises playing voices from the scene, starts pending sources and music, refills
    // streams, and drops voices whose node or source has gone. `game` is false in the editor,
    // where only previews play. `raycast` enables occlusion. Without an output device, renders
    // `delta_seconds` of audio so voices advance with the simulation.
    void update(const Scene& scene, bool game, double delta_seconds,
                const AudioRaycast& raycast = {});

    // Starts (or restarts) the entity's source. Outside the game it plays as a flat preview.
    // `volume_db` and `pitch` adjust this playing only, relative to the authored source.
    [[nodiscard]] bool play(const Scene& scene, Entity entity, bool game, std::string& error,
                            double volume_db = 0.0, double pitch = 1.0);
    // Stops the entity's source; returns whether it was playing.
    bool stop(Entity entity);
    void stop_all();
    [[nodiscard]] bool playing(Entity entity) const;
    // Seconds into the entity's source's clip, while it plays.
    [[nodiscard]] std::optional<double> position(Entity entity) const;

    // One-shots: a handle (never zero) for stopping and asking about the sound, or zero with
    // `error`. They end by themselves and with the game.
    [[nodiscard]] std::uint64_t play_clip(const Scene& scene, std::string_view clip,
                                          const AudioOneShot& options, std::string& error);
    bool stop_sound(std::uint64_t handle);
    [[nodiscard]] bool sound_playing(std::uint64_t handle) const;
    [[nodiscard]] std::optional<double> sound_position(std::uint64_t handle) const;

    // Music players, during the game. `track` -1 is the next in the playlist; `sync` chooses
    // when the change happens, defaulting to the player's own setting.
    [[nodiscard]] bool music_play(const Scene& scene, Entity entity, int track,
                                  std::optional<MusicPlayer::Sync> sync, std::string& error);
    bool music_stop(Entity entity, double fade_seconds);
    // The playlist index now playing, or -1.
    [[nodiscard]] int music_track(Entity entity) const;

    // Game-time mixer changes from scripts, undone when the game stops. Volume fades over
    // `fade_seconds`.
    [[nodiscard]] bool set_bus_volume(std::string_view bus, double volume_db, double fade_seconds,
                                      std::string& error);
    [[nodiscard]] bool set_bus_mute(std::string_view bus, bool mute, std::string& error);
    [[nodiscard]] bool set_bus_effect(std::string_view bus, std::size_t index,
                                      std::string_view parameter, double value, std::string& error);
    [[nodiscard]] std::optional<double> bus_volume(std::string_view bus) const;

    // Mixes the next frames into `output` (interleaved stereo at audio_output_rate). The device
    // thread calls it; tests call it to hear what the game would play.
    void render(std::span<float> output);
    [[nodiscard]] std::uint64_t rendered_frames() const;
    [[nodiscard]] std::string status_json(const Scene& scene) const;
    // Duration, format and `peaks` min/max pairs across the clip for waveform display. Long files
    // are summarised in one streaming pass instead of being kept.
    [[nodiscard]] std::optional<std::string> clip_json(std::string_view path, std::size_t peaks,
                                                       std::string& error);
    // The listener this update used, if any: a listener node, else the active camera.
    [[nodiscard]] std::optional<Entity> listener() const;
    // World-space reverb zone shapes and spatial source ranges, for the editor's viewport.
    [[nodiscard]] static std::string debug_shapes_json(const Scene& scene);

private:
    enum class Kind : std::uint8_t { source, one_shot, music };
    struct Playback {
        Kind kind{Kind::source};
        std::uint64_t voice{};
        Entity entity{};          // The source's or music player's node.
        std::string clip;
        bool game{};
        double volume_db{};
        double pitch{1.0};
        AudioSource settings;     // One-shots: their own spatial settings.
        std::optional<Vec3> position; // One-shots with a place in the world.
        int track{-1};            // Music: the playlist index.
        bool fading_out{};        // Music: on its way out after a change.
        double occlusion{};
        double occlusion_target{};
        std::optional<Vec3> last_position;
        std::shared_ptr<AudioStream> stream;
    };
    struct MusicState {
        std::uint64_t current{}; // Playback handle, or zero.
        int track{-1};
        std::vector<int> order;
        std::size_t order_index{};
        std::uint64_t plays{};
        bool started{};
        bool stopped{};
        bool advance_scheduled{};
    };
    struct BusOverride {
        std::optional<double> volume_db;
        double fade_from{}, fade_to{}, fade_elapsed{}, fade_duration{};
        std::optional<bool> mute;
        std::map<std::pair<std::size_t, std::string>, double> parameters;
    };
    struct Listener {
        Vec3 position{};
        Vec3 right{1, 0, 0};
        Vec3 up{0, 1, 0};
        Vec3 forward{0, 0, -1};
        Vec3 velocity{};
        std::optional<Entity> entity;
    };

    [[nodiscard]] AudioVoiceTarget target_for(const Scene& scene, const AudioSource& source,
                                              Playback& playback, double delta_seconds) const;
    [[nodiscard]] std::optional<Vec3> playback_position(const Scene& scene, const Playback& playback) const;
    void find_listener(const Scene& scene, double delta_seconds);
    // Starts a voice for `playback` from its clip, streaming long files. Returns the handle, or
    // zero with `error`. Takes the mixer's lock itself.
    std::uint64_t start(Playback playback, const AudioVoiceTarget& target, std::string& error,
                        std::uint64_t delay_frames = 0, float fade = 1.0F);
    bool start_source(const Scene& scene, Entity entity, bool game, std::string& error,
                      double volume_db = 0.0, double pitch = 1.0);
    void update_zones(const Scene& scene, bool game);
    void update_occlusion(const Scene& scene, double delta_seconds, const AudioRaycast& raycast);
    void update_music(const Scene& scene, double delta_seconds);
    void refill_streams();
    void apply_settings();
    [[nodiscard]] int next_track(const MusicPlayer& player, MusicState& state, bool& wrapped) const;
    bool change_track(const Scene& scene, Entity entity, int track, MusicPlayer::Sync sync,
                      std::string& error);
    [[nodiscard]] AudioVoiceTarget music_target(const MusicPlayer& player) const;

    // Guards the mixer (which the device thread renders) and the streams' rings.
    mutable std::mutex mixer_mutex_;
    AudioMixer mixer_;
    AudioClipCache clips_;
    AudioSettings base_settings_{default_audio_settings()};
    std::map<std::string, BusOverride, std::less<>> bus_overrides_;
    std::map<std::uint64_t, Playback> playbacks_;
    std::map<Entity, std::uint64_t> sources_; // Node source -> its playback handle.
    std::map<Entity, MusicState> music_;
    std::uint64_t next_handle_{1};
    // Nodes whose play_on_start has fired this game, so each starts once.
    std::set<Entity> started_;
    bool game_running_{};
    bool paused_{};
    Listener listener_;
    std::optional<Vec3> listener_last_position_;
    double pending_frames_{};
    std::size_t occlusion_cursor_{};
    // Reverb zones given a slot in the mixer, and how strongly the listener hears each.
    std::array<Entity, maximum_reverb_slots> zone_slots_{};
    std::vector<std::pair<Entity, double>> listener_zones_;
    std::vector<float> scratch_;
    std::string output_error_;
    struct Device;
    std::unique_ptr<Device> device_;
};

} // namespace relay
