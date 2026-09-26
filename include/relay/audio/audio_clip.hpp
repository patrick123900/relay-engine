#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// A whole sound decoded to 32-bit float PCM at its own sample rate, one or two interleaved
// channels. Sources with more channels are mixed down to stereo when decoded.
struct AudioClip {
    std::uint32_t sample_rate{};
    std::uint32_t channels{};
    std::vector<float> samples;
    [[nodiscard]] std::size_t frames() const { return channels ? samples.size() / channels : 0U; }
    [[nodiscard]] double duration_seconds() const {
        return sample_rate ? static_cast<double>(frames()) / sample_rate : 0.0;
    }
};

inline constexpr std::uintmax_t maximum_audio_file_bytes = 256ULL * 1024U * 1024U;
// About 11.6 minutes of 48 kHz stereo; files long enough to reach it stream instead (AudioStream).
inline constexpr std::size_t maximum_audio_clip_bytes = 256U * 1024U * 1024U;
inline constexpr std::size_t audio_clip_cache_bytes = 1024U * 1024U * 1024U;

// WAV, FLAC, MP3 and Ogg Vorbis, recognised by content rather than name.
[[nodiscard]] bool audio_file_extension(std::string_view filename);
[[nodiscard]] std::optional<AudioClip> decode_audio(const std::vector<std::uint8_t>& bytes,
                                                    std::string& error);
[[nodiscard]] std::optional<AudioClip> decode_audio_file(const std::filesystem::path& path,
                                                         std::string& error);

// A file's format and min/max peak pairs across it (the channels mixed), read in one pass
// without keeping the samples.
struct AudioFileSummary {
    std::uint32_t sample_rate{};
    std::uint32_t channels{};
    std::uint64_t frames{};
    std::vector<float> peaks;
};
[[nodiscard]] std::optional<AudioFileSummary> summarize_audio_file(const std::filesystem::path& path,
                                                                   std::size_t buckets,
                                                                   std::string& error);

// Files longer than this stream instead of decoding whole, so music of any length plays at once.
inline constexpr double audio_stream_threshold_seconds = 10.0;

// A long file decoded a little at a time into a four-second ring. The game thread calls
// decode() (outside the mixer's lock) and commit() (inside it); the mixer reads frames by their
// index in the continuous stream, which counts on across loops.
class AudioStream {
public:
    ~AudioStream();
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;
    [[nodiscard]] static std::shared_ptr<AudioStream> open(const std::filesystem::path& path,
                                                           bool loop, std::string& error);

    [[nodiscard]] std::uint32_t sample_rate() const { return sample_rate_; }
    [[nodiscard]] std::uint32_t channels() const { return channels_; }
    // Frames in one pass through the file; zero when the format cannot say.
    [[nodiscard]] std::uint64_t track_frames() const { return track_frames_; }
    void set_loop(bool loop) { loop_ = loop; }

    // Game thread: decodes into `scratch` as many frames as the ring has room for, given how far
    // the mixer has read. Returns the frames decoded; `finished` says the file has ended for good.
    std::size_t decode(std::vector<float>& scratch, std::uint64_t consumed, bool& finished);
    // Under the mixer's lock: appends decoded frames.
    void commit(const std::vector<float>& scratch, std::size_t frames, bool finished);

    // Under the mixer's lock.
    [[nodiscard]] std::uint64_t written() const { return written_; }
    [[nodiscard]] bool ended() const { return ended_; }
    [[nodiscard]] float sample(std::uint64_t frame, std::uint32_t channel) const {
        return frame < written_ && written_ - frame <= capacity_
                   ? ring_[(frame % capacity_) * channels_ + channel]
                   : 0.0F;
    }
    std::uint64_t consumed{};

private:
    AudioStream() = default;
    struct Decoder;
    std::unique_ptr<Decoder> decoder_;
    std::vector<float> ring_;
    std::uint64_t capacity_{};
    std::uint64_t written_{};
    std::uint64_t decoded_{}; // Game thread's own count, ahead of written_ until committed.
    std::uint64_t track_frames_{};
    std::uint32_t sample_rate_{}, channels_{};
    bool loop_{};
    bool ended_{};
    bool decoder_finished_{};
};

// Decoded clips by project-relative path, reloaded when the file changes. Clips no voice holds are
// evicted, least recently used first, once the cache exceeds its budget.
class AudioClipCache {
public:
    void set_root(std::optional<std::filesystem::path> root);
    [[nodiscard]] const std::optional<std::filesystem::path>& root() const { return root_; }
    [[nodiscard]] std::shared_ptr<const AudioClip> load(std::string_view path, std::string& error);
    // The file's safe location, or nothing (with `error`) for a path outside the project.
    [[nodiscard]] std::optional<std::filesystem::path> resolve(std::string_view path,
                                                               std::string& error) const;
    // Whether the file is long enough to stream; decided once per file version.
    [[nodiscard]] bool streams(std::string_view path, std::string& error);
    void clear();
    [[nodiscard]] std::size_t bytes() const { return bytes_; }

private:
    struct Entry {
        std::shared_ptr<const AudioClip> clip;
        std::filesystem::file_time_type modified{};
        std::uintmax_t size{};
        std::uint64_t used{};
    };
    void evict();

    std::optional<std::filesystem::path> root_;
    std::map<std::string, Entry, std::less<>> entries_;
    struct Length {
        std::filesystem::file_time_type modified{};
        std::uintmax_t size{};
        bool stream{};
    };
    std::map<std::string, Length, std::less<>> lengths_;
    std::size_t bytes_{};
    std::uint64_t clock_{};
};

} // namespace relay
