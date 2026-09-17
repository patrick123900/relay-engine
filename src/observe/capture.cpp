#include "relay/observe/capture.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace relay {
namespace {

#ifdef RELAY_FFMPEG_PATH
constexpr std::string_view ffmpeg_executable = RELAY_FFMPEG_PATH;
#else
constexpr std::string_view ffmpeg_executable = "ffmpeg";
#endif

void append_u32_be(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t crc32(const std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

void append_chunk(std::vector<std::uint8_t>& png, const std::array<char, 4>& type,
                  const std::span<const std::uint8_t> payload) {
    append_u32_be(png, static_cast<std::uint32_t>(payload.size()));
    const auto crc_start = png.size();
    for (const char value : type) png.push_back(static_cast<std::uint8_t>(value));
    png.insert(png.end(), payload.begin(), payload.end());
    append_u32_be(png, crc32(std::span<const std::uint8_t>{png}.subspan(crc_start)));
}

std::uint32_t adler32(const std::span<const std::uint8_t> bytes) {
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const auto byte : bytes) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16U) | a;
}

bool validate_frame(const FrameView frame, std::string& error) {
    const auto required = static_cast<std::uint64_t>(frame.width) * frame.height * 4U;
    if (frame.width == 0U || frame.height == 0U || required > frame.rgba.size() ||
        required > std::numeric_limits<std::uint32_t>::max()) {
        error = "frame dimensions or RGBA storage are invalid";
        return false;
    }
    return true;
}

bool write_png(const FrameView frame, const std::filesystem::path& path, std::string& error) {
    if (!validate_frame(frame, error)) return false;
    const auto row_bytes = static_cast<std::size_t>(frame.width) * 4U;
    std::vector<std::uint8_t> filtered;
    filtered.reserve((row_bytes + 1U) * frame.height);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        filtered.push_back(0U);
        const auto start = static_cast<std::size_t>(y) * row_bytes;
        filtered.insert(filtered.end(), frame.rgba.begin() + static_cast<std::ptrdiff_t>(start),
                        frame.rgba.begin() + static_cast<std::ptrdiff_t>(start + row_bytes));
    }
    std::vector<std::uint8_t> compressed{0x78U, 0x01U};
    std::size_t offset = 0;
    while (offset < filtered.size()) {
        const auto count = std::min<std::size_t>(65'535U, filtered.size() - offset);
        compressed.push_back(offset + count == filtered.size() ? 1U : 0U);
        compressed.push_back(static_cast<std::uint8_t>(count));
        compressed.push_back(static_cast<std::uint8_t>(count >> 8U));
        const auto inverse = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(count));
        compressed.push_back(static_cast<std::uint8_t>(inverse));
        compressed.push_back(static_cast<std::uint8_t>(inverse >> 8U));
        compressed.insert(compressed.end(), filtered.begin() + static_cast<std::ptrdiff_t>(offset),
                          filtered.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    append_u32_be(compressed, adler32(filtered));

    std::vector<std::uint8_t> png{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    std::vector<std::uint8_t> header;
    append_u32_be(header, frame.width);
    append_u32_be(header, frame.height);
    header.insert(header.end(), {8U, 6U, 0U, 0U, 0U});
    append_chunk(png, {'I', 'H', 'D', 'R'}, header);
    append_chunk(png, {'I', 'D', 'A', 'T'}, compressed);
    append_chunk(png, {'I', 'E', 'N', 'D'}, {});
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!output) {
        error = "PNG capture write failed";
        return false;
    }
    return true;
}

void write_u16(std::ostream& stream, const std::uint16_t value) {
    const std::array bytes{static_cast<char>(value), static_cast<char>(value >> 8U)};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ostream& stream, const std::uint32_t value) {
    const std::array bytes{static_cast<char>(value), static_cast<char>(value >> 8U),
                           static_cast<char>(value >> 16U), static_cast<char>(value >> 24U)};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool write_bmp(const FrameView frame, const std::filesystem::path& path, std::string& error) {
    if (!validate_frame(frame, error)) return false;
    constexpr std::uint32_t header_size = 54U;
    const auto pixel_bytes = frame.width * frame.height * 4U;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.put('B'); output.put('M'); write_u32(output, header_size + pixel_bytes);
    write_u16(output, 0U); write_u16(output, 0U); write_u32(output, header_size);
    write_u32(output, 40U); write_u32(output, frame.width); write_u32(output, frame.height);
    write_u16(output, 1U); write_u16(output, 32U); write_u32(output, 0U);
    write_u32(output, pixel_bytes); write_u32(output, 2835U); write_u32(output, 2835U);
    write_u32(output, 0U); write_u32(output, 0U);
    for (std::uint32_t row = frame.height; row > 0; --row) {
        const auto y = row - 1U;
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const auto index = (static_cast<std::size_t>(y) * frame.width + x) * 4U;
            const std::array pixel{static_cast<char>(frame.rgba[index + 2U]),
                                   static_cast<char>(frame.rgba[index + 1U]),
                                   static_cast<char>(frame.rgba[index]),
                                   static_cast<char>(frame.rgba[index + 3U])};
            output.write(pixel.data(), static_cast<std::streamsize>(pixel.size()));
        }
    }
    if (!output) {
        error = "BMP capture write failed";
        return false;
    }
    return true;
}

} // namespace

bool write_frame_image(const FrameView frame, const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::error_code filesystem_error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "could not create capture directory: " + filesystem_error.message();
        return false;
    }
    if (path.extension() == ".png") return write_png(frame, path, error);
    if (path.extension() == ".bmp") return write_bmp(frame, path, error);
    error = "capture path must end in .png or .bmp";
    return false;
}

bool video_encoder_available() {
#ifdef RELAY_FFMPEG_PATH
    return true;
#else
    return false;
#endif
}

CaptureQueue::CaptureQueue(const std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1U)),
                                                         worker_([this] { run(); }) {}

CaptureQueue::~CaptureQueue() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    worker_.join();
}

std::uint64_t CaptureQueue::submit(const FrameView frame, std::filesystem::path path,
                                   std::string& error) {
    if (!validate_frame(frame, error)) return 0;
    const auto id = reserve(std::move(path), "deterministic", error);
    if (id) deliver(id, {frame.width, frame.height, {frame.rgba.begin(), frame.rgba.end()}});
    return id;
}

std::uint64_t CaptureQueue::reserve(std::filesystem::path path, std::string source, std::string& error) {
    std::scoped_lock lock(mutex_);
    std::size_t active = 0;
    for (const auto& [id, job] : jobs_) {
        (void)id;
        if (job.status.state == CaptureJobState::queued || job.status.state == CaptureJobState::writing) ++active;
    }
    if (stopping_ || active >= capacity_) { error = "capture queue is full"; return 0; }
    while (jobs_.size() >= 4096U) {
        auto found = std::find_if(jobs_.begin(), jobs_.end(), [](const auto& entry) {
            return entry.second.status.state == CaptureJobState::complete || entry.second.status.state == CaptureJobState::failed;
        });
        if (found == jobs_.end()) break;
        jobs_.erase(found);
    }
    const auto id = next_id_++;
    jobs_.emplace(id, Job{{id, CaptureJobState::queued, std::move(path), {}, std::move(source)}, {}});
    error.clear();
    return id;
}

void CaptureQueue::deliver(std::uint64_t id, OwnedFrame frame, std::string error) {
    std::scoped_lock lock(mutex_);
    auto& job = jobs_.at(id);
    if (job.status.state != CaptureJobState::queued) return;
    if (!error.empty() || !validate_frame(frame.view(), error)) {
        job.status.state = CaptureJobState::failed;
        job.status.error = std::move(error);
    } else {
        job.frame = std::move(frame);
        pending_.push_back(id);
        ready_.notify_one();
    }
    idle_.notify_all();
}

CaptureJobStatus CaptureQueue::status(const std::uint64_t id) const {
    std::scoped_lock lock(mutex_);
    const auto found = jobs_.find(id);
    return found == jobs_.end() ? CaptureJobStatus{id, CaptureJobState::failed, {}, "capture job does not exist"}
                               : found->second.status;
}

bool CaptureQueue::cancel(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    const auto found = jobs_.find(id);
    if (found == jobs_.end() || found->second.status.state != CaptureJobState::queued) return false;
    found->second.status.state = CaptureJobState::failed;
    found->second.status.error = "capture cancelled";
    found->second.frame = {};
    std::erase(pending_, id);
    idle_.notify_all();
    return true;
}

void CaptureQueue::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] {
        return std::none_of(jobs_.begin(), jobs_.end(), [](const auto& entry) {
            return entry.second.status.state == CaptureJobState::queued || entry.second.status.state == CaptureJobState::writing;
        });
    });
}

void CaptureQueue::run() {
    while (true) {
        std::uint64_t id = 0;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_ && pending_.empty()) return;
            id = pending_.front();
            pending_.pop_front();
            writing_ = true;
            jobs_.at(id).status.state = CaptureJobState::writing;
        }
        std::string error;
        Job* job = nullptr;
        {
            std::scoped_lock lock(mutex_);
            job = &jobs_.at(id);
        }
        const bool written = write_frame_image(job->frame.view(), job->status.path, error);
        {
            std::scoped_lock lock(mutex_);
            auto& status = jobs_.at(id).status;
            status.state = written ? CaptureJobState::complete : CaptureJobState::failed;
            status.error = std::move(error);
            jobs_.at(id).frame.rgba.clear();
            jobs_.at(id).frame.rgba.shrink_to_fit();
            writing_ = false;
        }
        idle_.notify_all();
    }
}

VideoRecorder::VideoRecorder(CaptureQueue& captures) : captures_(captures) {}
VideoRecorder::~VideoRecorder() { if (finalization_.valid()) finalization_.wait(); }

bool VideoRecorder::start(std::filesystem::path path, const std::uint32_t fps,
                          const std::uint32_t maximum_frames, const double fixed_delta_seconds,
                          std::string& error) {
    if (finalization_.valid() && finalization_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        error = "the previous video is still finalizing"; return false;
    }
    finalization_ = {};
    if (!video_encoder_available()) {
        error = "FFmpeg was not found when Relay was configured; WebM recording is unavailable";
        return false;
    }
    if (status_.recording) {
        error = "a video is already recording";
        return false;
    }
    if (path.extension() != ".webm" || fps == 0U || fps > 60U || maximum_frames == 0U ||
        maximum_frames > 3600U || fixed_delta_seconds <= 0.0) {
        error = "video requires .webm output, 1-60 fps and 1-3600 frames";
        return false;
    }
    static std::atomic<std::uint64_t> serial{0};
    temporary_directory_ = path.parent_path() /
                           (".relay-video-" + std::to_string(serial.fetch_add(1U)));
    std::error_code filesystem_error;
    std::filesystem::create_directories(temporary_directory_, filesystem_error);
    if (filesystem_error) {
        error = "could not create video frame directory: " + filesystem_error.message();
        return false;
    }
    status_ = {"deterministic", false, {}, true, std::move(path), fps, 0U, 0U};
    maximum_frames_ = maximum_frames;
    source_fps_ = 1.0 / fixed_delta_seconds;
    sampling_accumulator_ = source_fps_;
    width_ = height_ = 0;
    jobs_.clear();
    error.clear();
    return true;
}

bool VideoRecorder::sample_due() {
    if (!status_.recording || status_.submitted_frames >= maximum_frames_) return false;
    sampling_accumulator_ += static_cast<double>(status_.fps);
    if (sampling_accumulator_ < source_fps_) return false;
    sampling_accumulator_ -= source_fps_;
    return true;
}

void VideoRecorder::record(const FrameView frame) {
    if (sample_due()) record_sample(frame);
}

void VideoRecorder::record_sample(const FrameView frame) {
    if (!status_.recording || status_.submitted_frames >= maximum_frames_) return;
    if (width_ && (width_ != frame.width || height_ != frame.height)) { ++status_.dropped_frames; return; }
    width_ = frame.width; height_ = frame.height;
    std::ostringstream filename;
    filename << "frame-" << std::setw(8) << std::setfill('0') << status_.submitted_frames << ".png";
    std::string error;
    const auto job = captures_.submit(frame, temporary_directory_ / filename.str(), error);
    if (job == 0U) {
        ++status_.dropped_frames;
        return;
    }
    jobs_.push_back(job);
    ++status_.submitted_frames;
}

bool VideoRecorder::stop(std::string& error) {
    if (!status_.recording) {
        error = "no video is recording";
        return false;
    }
    status_.recording = false;
    finalization_ = std::async(std::launch::async, [this] { return finalize(); }).share();
    error.clear();
    return true;
}

std::string VideoRecorder::finalize() {
    std::string error;
    captures_.wait_idle();
    for (const auto job : jobs_) {
        const auto capture = captures_.status(job);
        if (capture.state != CaptureJobState::complete) {
            error = "video frame capture failed: " + capture.error;
            return error;
        }
    }
    if (jobs_.empty()) {
        error = "video has no captured frames";
        return error;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(status_.path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "could not create video output directory";
        return error;
    }
#ifdef _WIN32
    const auto command = "\"" + std::string(ffmpeg_executable) +
                         "\" -hide_banner -loglevel error -y -framerate " +
                         std::to_string(status_.fps) + " -i \"" +
                         (temporary_directory_ / "frame-%08d.png").string() +
                         "\" -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" -c:v libvpx-vp9 -pix_fmt yuv420p \"" + status_.path.string() + "\"";
#else
    auto quote = [](const std::string& value) {
        std::string output{"'"};
        for (const char character : value) output += character == '\'' ? "'\\''" : std::string(1, character);
        return output + '\'';
    };
    const auto command = quote(std::string(ffmpeg_executable)) +
                         " -hide_banner -loglevel error -y -framerate " +
                         std::to_string(status_.fps) + " -i " +
                         quote((temporary_directory_ / "frame-%08d.png").string()) +
                         " -vf 'pad=ceil(iw/2)*2:ceil(ih/2)*2' -c:v libvpx-vp9 -pix_fmt yuv420p " + quote(status_.path.string());
#endif
    if (std::system(command.c_str()) != 0) {
        error = "FFmpeg WebM encoding failed; captured PNG frames remain in " +
                temporary_directory_.string();
        return error;
    }
    std::filesystem::remove_all(temporary_directory_, filesystem_error);
    error.clear();
    return {};
}

VideoStatus VideoRecorder::status() const {
    auto result = status_;
    if (finalization_.valid()) {
        result.finalizing = finalization_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        if (!result.finalizing) result.error = finalization_.get();
    }
    return result;
}

} // namespace relay
