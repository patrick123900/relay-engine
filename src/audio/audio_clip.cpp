#include "relay/audio/audio_clip.hpp"
#include "relay/scene/project.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>

namespace relay {
namespace {

// Opens `bytes` as floats at the source rate, with at most two channels.
bool open_decoder(const std::vector<std::uint8_t>& bytes, ma_decoder& decoder,
                  const ma_uint32 channels) {
    const auto config = ma_decoder_config_init(ma_format_f32, channels, 0);
    return ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder) == MA_SUCCESS;
}

} // namespace

bool audio_file_extension(const std::string_view filename) {
    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string extension(filename.substr(dot + 1U));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == "wav" || extension == "flac" || extension == "mp3" || extension == "ogg";
}

std::optional<AudioClip> decode_audio(const std::vector<std::uint8_t>& bytes, std::string& error) {
    if (bytes.empty()) {
        error = "the audio file is empty";
        return std::nullopt;
    }
    ma_decoder decoder;
    if (!open_decoder(bytes, decoder, 0)) {
        error = "not a readable WAV, FLAC, MP3 or Ogg Vorbis file";
        return std::nullopt;
    }
    if (decoder.outputChannels > 2U) {
        ma_decoder_uninit(&decoder);
        if (!open_decoder(bytes, decoder, 2)) {
            error = "could not mix the audio file down to stereo";
            return std::nullopt;
        }
    }
    AudioClip clip;
    clip.channels = decoder.outputChannels;
    clip.sample_rate = decoder.outputSampleRate;
    if (clip.channels == 0U || clip.sample_rate < 8000U || clip.sample_rate > 384000U) {
        ma_decoder_uninit(&decoder);
        error = "unsupported channel count or sample rate";
        return std::nullopt;
    }
    const std::size_t maximum_samples = maximum_audio_clip_bytes / sizeof(float);
    std::array<float, 4096> chunk{};
    const ma_uint64 chunk_frames = chunk.size() / clip.channels;
    for (;;) {
        ma_uint64 read = 0;
        const auto result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), chunk_frames, &read);
        const auto count = static_cast<std::size_t>(read) * clip.channels;
        if (clip.samples.size() + count > maximum_samples) {
            ma_decoder_uninit(&decoder);
            error = "the decoded sound is longer than Relay's clip limit (256 MiB of samples)";
            return std::nullopt;
        }
        clip.samples.insert(clip.samples.end(), chunk.begin(),
                            chunk.begin() + static_cast<std::ptrdiff_t>(count));
        if (result != MA_SUCCESS || read < chunk_frames) break;
    }
    ma_decoder_uninit(&decoder);
    if (clip.samples.empty()) {
        error = "the audio file holds no samples";
        return std::nullopt;
    }
    return clip;
}

std::optional<AudioClip> decode_audio_file(const std::filesystem::path& path, std::string& error) {
    std::error_code failure;
    const auto size = std::filesystem::file_size(path, failure);
    if (failure || !std::filesystem::is_regular_file(path, failure)) {
        error = "audio file not found";
        return std::nullopt;
    }
    if (size > maximum_audio_file_bytes) {
        error = "the audio file is larger than 256 MiB";
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!input || !input.read(reinterpret_cast<char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()))) {
        error = "could not read the audio file";
        return std::nullopt;
    }
    return decode_audio(bytes, error);
}

void AudioClipCache::set_root(std::optional<std::filesystem::path> root) {
    if (root == root_) return;
    root_ = std::move(root);
    clear();
}

void AudioClipCache::clear() {
    entries_.clear();
    lengths_.clear();
    bytes_ = 0;
}

std::optional<AudioFileSummary> summarize_audio_file(const std::filesystem::path& path,
                                                     const std::size_t buckets, std::string& error) {
    ma_decoder decoder;
    const auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (ma_decoder_init_file(path.string().c_str(), &config, &decoder) != MA_SUCCESS) {
        error = "not a readable WAV, FLAC, MP3 or Ogg Vorbis file";
        return std::nullopt;
    }
    AudioFileSummary summary;
    summary.channels = decoder.outputChannels;
    summary.sample_rate = decoder.outputSampleRate;
    std::array<float, 8192> chunk{};
    const ma_uint64 chunk_frames = chunk.size() / std::max<ma_uint32>(summary.channels, 1U);
    const auto read_all = [&](const auto& visit) {
        std::uint64_t at = 0;
        for (;;) {
            ma_uint64 read = 0;
            const auto result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), chunk_frames, &read);
            for (ma_uint64 frame = 0; frame < read; ++frame) {
                float value = 0.0F;
                for (std::uint32_t channel = 0; channel < summary.channels; ++channel)
                    value += chunk[frame * summary.channels + channel];
                visit(at++, value / static_cast<float>(summary.channels));
            }
            if (result != MA_SUCCESS || read < chunk_frames) break;
        }
        return at;
    };
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) != MA_SUCCESS || length == 0) {
        length = read_all([](std::uint64_t, float) {});
        (void)ma_decoder_seek_to_pcm_frame(&decoder, 0);
    }
    summary.frames = length;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buckets, length));
    summary.peaks.assign(count * 2U, 0.0F);
    if (count > 0U)
        (void)read_all([&](const std::uint64_t frame, const float value) {
            const auto bucket = std::min<std::size_t>(count - 1U, static_cast<std::size_t>(frame * count / length));
            summary.peaks[bucket * 2U] = std::min(summary.peaks[bucket * 2U], value);
            summary.peaks[bucket * 2U + 1U] = std::max(summary.peaks[bucket * 2U + 1U], value);
        });
    ma_decoder_uninit(&decoder);
    return summary;
}

struct AudioStream::Decoder {
    ma_decoder decoder{};
    ~Decoder() { ma_decoder_uninit(&decoder); }
};

AudioStream::~AudioStream() = default;

std::shared_ptr<AudioStream> AudioStream::open(const std::filesystem::path& path, const bool loop,
                                               std::string& error) {
    std::shared_ptr<AudioStream> stream(new AudioStream());
    auto decoder = std::make_unique<Decoder>();
    auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (ma_decoder_init_file(path.string().c_str(), &config, &decoder->decoder) != MA_SUCCESS) {
        error = "not a readable WAV, FLAC, MP3 or Ogg Vorbis file";
        return nullptr;
    }
    if (decoder->decoder.outputChannels > 2U) {
        ma_decoder_uninit(&decoder->decoder);
        config = ma_decoder_config_init(ma_format_f32, 2, 0);
        if (ma_decoder_init_file(path.string().c_str(), &config, &decoder->decoder) != MA_SUCCESS) {
            error = "could not mix the audio file down to stereo";
            return nullptr;
        }
    }
    stream->channels_ = decoder->decoder.outputChannels;
    stream->sample_rate_ = decoder->decoder.outputSampleRate;
    if (stream->channels_ == 0U || stream->sample_rate_ < 8000U || stream->sample_rate_ > 384000U) {
        error = "unsupported channel count or sample rate";
        return nullptr;
    }
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder->decoder, &length) == MA_SUCCESS)
        stream->track_frames_ = length;
    // Four seconds covers the fastest playback (4x pitch, 2x doppler) between 60 Hz game steps
    // many times over, and a hitch of a second or two at normal speed.
    stream->capacity_ = static_cast<std::uint64_t>(stream->sample_rate_) * 4U;
    stream->ring_.assign(stream->capacity_ * stream->channels_, 0.0F);
    stream->loop_ = loop;
    stream->decoder_ = std::move(decoder);
    return stream;
}

std::size_t AudioStream::decode(std::vector<float>& scratch, const std::uint64_t consumed_frames,
                                bool& finished) {
    finished = decoder_finished_;
    const auto buffered = decoded_ - std::min(decoded_, consumed_frames);
    if (decoder_finished_ || buffered >= capacity_) return 0;
    const auto room = static_cast<std::size_t>(capacity_ - buffered);
    scratch.resize(room * channels_);
    std::size_t total = 0;
    // A few passes: a loop point in the middle of the room means reading on from the start.
    for (int pass = 0; pass < 4 && total < room; ++pass) {
        ma_uint64 read = 0;
        const auto result = ma_decoder_read_pcm_frames(&decoder_->decoder, scratch.data() + total * channels_,
                                                       room - total, &read);
        total += static_cast<std::size_t>(read);
        if (total >= room) break;
        if (result == MA_SUCCESS && read > 0) continue;
        // The end of the file.
        if (track_frames_ == 0U) track_frames_ = decoded_ + total;
        if (!loop_ || ma_decoder_seek_to_pcm_frame(&decoder_->decoder, 0) != MA_SUCCESS) {
            decoder_finished_ = true;
            break;
        }
    }
    decoded_ += total;
    finished = decoder_finished_;
    return total;
}

void AudioStream::commit(const std::vector<float>& scratch, const std::size_t frames,
                         const bool finished) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto slot = static_cast<std::size_t>((written_ + frame) % capacity_);
        for (std::uint32_t channel = 0; channel < channels_; ++channel)
            ring_[slot * channels_ + channel] = scratch[frame * channels_ + channel];
    }
    written_ += frames;
    ended_ = finished;
}

std::optional<std::filesystem::path> AudioClipCache::resolve(const std::string_view path,
                                                             std::string& error) const {
    if (!audio_file_extension(path)) {
        error = "audio clips must be .wav, .flac, .mp3 or .ogg files";
        return std::nullopt;
    }
    auto resolved =
        workspace_file((root_ ? *root_ : std::filesystem::path{"."}).generic_string(), path, "");
    if (!resolved) error = "audio clip path must be a safe project-relative name";
    return resolved;
}

bool AudioClipCache::streams(const std::string_view path, std::string& error) {
    const auto resolved = resolve(path, error);
    if (!resolved) return false;
    std::error_code failure;
    const auto modified = std::filesystem::last_write_time(*resolved, failure);
    const auto size = failure ? 0U : std::filesystem::file_size(*resolved, failure);
    if (failure) {
        error = "audio clip " + std::string(path) + " not found";
        return false;
    }
    if (const auto found = lengths_.find(path);
        found != lengths_.end() && found->second.modified == modified && found->second.size == size)
        return found->second.stream;
    // Opening a decoder reads only the header (MP3 is scanned for its length).
    ma_decoder decoder;
    const auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
    bool stream = false;
    if (ma_decoder_init_file(resolved->string().c_str(), &config, &decoder) == MA_SUCCESS) {
        ma_uint64 length = 0;
        const bool known = ma_decoder_get_length_in_pcm_frames(&decoder, &length) == MA_SUCCESS && length;
        stream = known ? static_cast<double>(length) / decoder.outputSampleRate > audio_stream_threshold_seconds
                       : size > 4U * 1024U * 1024U;
        ma_decoder_uninit(&decoder);
    }
    lengths_[std::string(path)] = Length{modified, size, stream};
    return stream;
}

std::shared_ptr<const AudioClip> AudioClipCache::load(const std::string_view path,
                                                      std::string& error) {
    const auto resolved = resolve(path, error);
    if (!resolved) return nullptr;
    std::error_code failure;
    const auto modified = std::filesystem::last_write_time(*resolved, failure);
    const auto size = failure ? 0U : std::filesystem::file_size(*resolved, failure);
    if (failure) {
        error = "audio clip " + std::string(path) + " not found";
        return nullptr;
    }
    const auto found = entries_.find(path);
    if (found != entries_.end() && found->second.modified == modified &&
        found->second.size == size) {
        found->second.used = ++clock_;
        return found->second.clip;
    }
    auto decoded = decode_audio_file(*resolved, error);
    if (!decoded) {
        error = std::string(path) + ": " + error;
        return nullptr;
    }
    auto clip = std::make_shared<const AudioClip>(std::move(*decoded));
    if (found != entries_.end()) {
        bytes_ -= found->second.clip->samples.size() * sizeof(float);
        entries_.erase(found);
    }
    bytes_ += clip->samples.size() * sizeof(float);
    entries_.emplace(std::string(path), Entry{clip, modified, size, ++clock_});
    evict();
    return clip;
}

void AudioClipCache::evict() {
    while (bytes_ > audio_clip_cache_bytes) {
        auto oldest = entries_.end();
        for (auto item = entries_.begin(); item != entries_.end(); ++item)
            if (item->second.clip.use_count() == 1 &&
                (oldest == entries_.end() || item->second.used < oldest->second.used))
                oldest = item;
        if (oldest == entries_.end()) return;
        bytes_ -= oldest->second.clip->samples.size() * sizeof(float);
        entries_.erase(oldest);
    }
}

} // namespace relay
