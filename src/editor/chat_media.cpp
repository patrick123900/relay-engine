#include "relay/editor/chat_media.hpp"
#include "relay/render/image_decode.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <vector>
#include <cstring>
#include <cmath>
#ifdef RELAY_CHAT_VIDEO
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#endif
namespace relay {
namespace {
struct Frame { TextureAsset image; double duration{}; std::string error; };
std::filesystem::path resolve(std::string_view link) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto target = fs::weakly_canonical(fs::path(link), ec);
    if (ec) return {};
    for (const auto& root : {fs::path("captures"), fs::path(".relay/chat-files")}) {
        if (fs::is_symlink(root, ec) || (root.string().starts_with(".relay") && fs::is_symlink(".relay", ec))) continue;
        const auto base = fs::weakly_canonical(root, ec);
        if (ec) continue;
        const auto relative = target.lexically_relative(base);
        if (!relative.empty() && *relative.begin() != ".." && target != base) return target;
    }
    return {};
}
Frame decode(const std::filesystem::path& filename, double position, bool video, unsigned maximum_dimension) {
    Frame result;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(filename, ec);
    if (ec || bytes > 128U * 1024U * 1024U) { result.error = "Media file unavailable or exceeds 128 MB."; return result; }
    std::ifstream input(filename, std::ios::binary);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(bytes));
    if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) { result.error = "Cannot read media."; return result; }
    if (!video) {
        if (!decode_image_rgba(data, filename.extension().string(), result.image, result.error)) return result;
        if (result.image.width > maximum_dimension || result.image.height > maximum_dimension) {
            const auto source = std::move(result.image.rgba);
            const auto old_width = result.image.width, old_height = result.image.height;
            const double scale = static_cast<double>(maximum_dimension) / std::max(old_width, old_height);
            result.image.width = std::max(1U, static_cast<unsigned>(old_width * scale));
            result.image.height = std::max(1U, static_cast<unsigned>(old_height * scale));
            result.image.rgba.resize(static_cast<std::size_t>(result.image.width) * result.image.height * 4);
            for (unsigned y = 0; y < result.image.height; ++y) for (unsigned x = 0; x < result.image.width; ++x)
                std::memcpy(result.image.rgba.data() + (static_cast<std::size_t>(y) * result.image.width + x) * 4,
                    source.data() + (static_cast<std::size_t>(y * old_height / result.image.height) * old_width + x * old_width / result.image.width) * 4, 4);
        }
        return result;
    }
#ifdef RELAY_CHAT_VIDEO
    struct Memory { const std::vector<std::uint8_t>& data; std::int64_t offset{}; } memory{data};
    auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
    auto* io = avio_alloc_context(buffer, 32768, 0, &memory, [](void* opaque, std::uint8_t* destination, int size) {
        auto& source = *static_cast<Memory*>(opaque);
        const auto count = std::min<std::int64_t>(size, static_cast<std::int64_t>(source.data.size()) - source.offset);
        if (count <= 0) return AVERROR_EOF;
        std::memcpy(destination, source.data.data() + source.offset, static_cast<std::size_t>(count)); source.offset += count;
        return static_cast<int>(count);
    }, nullptr, [](void* opaque, std::int64_t offset, int whence) -> std::int64_t {
        auto& source = *static_cast<Memory*>(opaque);
        if (whence == AVSEEK_SIZE) return static_cast<std::int64_t>(source.data.size());
        const auto destination = (whence == SEEK_CUR ? source.offset : whence == SEEK_END ? static_cast<std::int64_t>(source.data.size()) : 0) + offset;
        if (destination < 0 || destination > static_cast<std::int64_t>(source.data.size())) return AVERROR(EINVAL);
        return source.offset = destination;
    });
    auto* format = avformat_alloc_context();
    if (!io || !format) { if (io) { av_freep(&io->buffer); avio_context_free(&io); } else av_free(buffer); if (format) avformat_free_context(format); result.error = "Video allocation failed."; return result; }
    format->pb = io; format->flags |= AVFMT_FLAG_CUSTOM_IO;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "", 0);
    // Only self-contained supported containers; playlists cannot load other files or URLs.
    const auto* demuxer = av_find_input_format(filename.extension() == ".webm" ? "matroska" : "mov");
    AVCodecContext* codec = nullptr; AVFrame* frame = nullptr; AVFrame* last_frame = nullptr; AVPacket* packet = nullptr; SwsContext* scaler = nullptr;
    do {
        if (avformat_open_input(&format, nullptr, demuxer, &options) < 0 || avformat_find_stream_info(format, nullptr) < 0) break;
        const int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_index < 0) break;
        auto* stream = format->streams[stream_index];
        const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        codec = avcodec_alloc_context3(decoder);
        if (!codec || avcodec_parameters_to_context(codec, stream->codecpar) < 0 || codec->width <= 0 || codec->height <= 0 || codec->width > 4096 || codec->height > 4096) break;
        codec->thread_count = 1;
        if (avcodec_open2(codec, decoder, nullptr) < 0) break;
        result.duration = format->duration > 0 ? static_cast<double>(format->duration) / AV_TIME_BASE : 0;
        const auto target = static_cast<std::int64_t>(position / av_q2d(stream->time_base));
        if (position > 0) av_seek_frame(format, stream_index, target, AVSEEK_FLAG_BACKWARD);
        frame = av_frame_alloc(); last_frame = av_frame_alloc(); packet = av_packet_alloc();
        if (!frame || !last_frame || !packet) break;
        bool found = false;
        // Bound corrupt files and exceptionally long GOP decoding work.
        for (int count = 0; count < 3000 && !found; ++count) {
            const bool exhausted = av_read_frame(format, packet) < 0;
            if (exhausted) { avcodec_send_packet(codec, nullptr); }
            else { if (packet->stream_index == stream_index) avcodec_send_packet(codec, packet); av_packet_unref(packet); }
            while (avcodec_receive_frame(codec, frame) == 0) {
                av_frame_unref(last_frame); av_frame_ref(last_frame, frame);
                if (frame->best_effort_timestamp >= target || target == 0) { found = true; break; }
            }
            if (exhausted && !found) { if (last_frame->data[0]) { av_frame_unref(frame); av_frame_ref(frame, last_frame); found = true; } break; }
        }
        if (!found) break;
        const float scale = std::min(1.0F, 1024.0F / static_cast<float>(std::max(frame->width, frame->height)));
        result.image.width = static_cast<std::uint32_t>(std::max(1, static_cast<int>(static_cast<float>(frame->width) * scale)));
        result.image.height = static_cast<std::uint32_t>(std::max(1, static_cast<int>(static_cast<float>(frame->height) * scale)));
        result.image.rgba.resize(static_cast<std::size_t>(result.image.width) * result.image.height * 4);
        scaler = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), static_cast<int>(result.image.width), static_cast<int>(result.image.height), AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) { result.image = {}; break; }
        std::uint8_t* planes[]{result.image.rgba.data()}; int strides[]{static_cast<int>(result.image.width * 4)};
        sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
    } while (false);
    sws_freeContext(scaler); av_packet_free(&packet); av_frame_free(&frame); av_frame_free(&last_frame); avcodec_free_context(&codec);
    avformat_close_input(&format); av_dict_free(&options); av_freep(&io->buffer); avio_context_free(&io);
    if (result.image.rgba.empty()) result.error = "Video could not be decoded.";
#else
    (void)position; result.error = "Video playback is unavailable in this build.";
#endif
    return result;
}
}
struct ChatMedia::Impl {
    struct Entry {
        std::filesystem::path path;
        std::future<Frame> pending;
        std::unique_ptr<ImTextureData> texture;
        std::string error;
        bool video{}, playing{}, large{};
        float position{}, duration{};
        double updated{};
        float requested{};
        int used_frame{};
    };
    struct Retired { std::unique_ptr<ImTextureData> texture; int age{}; };
    std::map<std::string, Entry> entries;
    std::vector<Retired> retired;
    int retirement_frame{-1};
    std::string viewing;
    bool open_pending{};
    float zoom{1};
    ImVec2 pan{};
    void collect(bool headless) {
        for (auto it = retired.begin(); it != retired.end();) {
            auto& old = *it;
            if (retirement_frame != ImGui::GetFrameCount()) ++old.age;
            old.texture->UnusedFrames = old.age;
            if (headless || old.texture->Status == ImTextureStatus_Destroyed) {
                ImGui::UnregisterUserTexture(old.texture.get()); it = retired.erase(it);
            } else ++it;
        }
        retirement_frame = ImGui::GetFrameCount();
    }
    void update(Entry& entry) {
        if (entry.pending.valid() && entry.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto frame = entry.pending.get(); entry.error = frame.error; if (!entry.error.empty()) entry.playing = false; entry.duration = static_cast<float>(frame.duration);
            if (!frame.image.rgba.empty()) {
                retire(std::move(entry.texture)); entry.texture = std::make_unique<ImTextureData>();
                entry.texture->Create(ImTextureFormat_RGBA32, static_cast<int>(frame.image.width), static_cast<int>(frame.image.height));
                std::memcpy(entry.texture->Pixels, frame.image.rgba.data(), frame.image.rgba.size());
                entry.texture->RefCount = 1; entry.texture->SetStatus(ImTextureStatus_WantCreate); ImGui::RegisterUserTexture(entry.texture.get());
            }
        }
    }
    void controls(Entry& entry) {
        bool seek = false;
        if (ImGui::Button(entry.playing ? "Pause" : "Play")) { entry.playing = !entry.playing; if (entry.playing && entry.position >= entry.duration - .1F) { entry.position = 0; seek = true; } if (entry.playing) for (auto& [key, other] : entries) { (void)key; if (&other != &entry) other.playing = false; } entry.updated = ImGui::GetTime(); }
        ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##Position", &entry.position, 0, std::max(.001F, entry.duration), "%.1f s")) { seek = true; entry.updated = ImGui::GetTime(); }
        const auto now = ImGui::GetTime();
        if (entry.playing && now - entry.updated >= .1 && !entry.pending.valid()) { entry.position += static_cast<float>(now - entry.updated); entry.updated = now; if (entry.position >= entry.duration) { entry.position = entry.duration; entry.playing = false; } seek = true; }
        if (seek && !entry.pending.valid() && !entry.playing && entry.position == entry.duration) entry.position = std::max(0.0F, entry.duration - .05F);
        if ((seek || std::abs(entry.position - entry.requested) > .05F) && !entry.pending.valid()) {
            entry.requested = entry.position;
            entry.pending = std::async(std::launch::async, decode, entry.path, static_cast<double>(entry.position), true, 1024U);
        }
    }
    void retire(std::unique_ptr<ImTextureData> texture) { if (texture) { texture->SetStatus(ImTextureStatus_WantDestroy); retired.push_back({std::move(texture), 0}); } }
};
ChatMedia::ChatMedia() : impl_(std::make_unique<Impl>()) {}
ChatMedia::~ChatMedia() = default;
void ChatMedia::clear() {
    for (auto& [key, entry] : impl_->entries) { (void)key; if (entry.texture) ImGui::UnregisterUserTexture(entry.texture.get()); }
    for (auto& retired : impl_->retired) ImGui::UnregisterUserTexture(retired.texture.get());
    impl_->entries.clear(); impl_->retired.clear(); impl_->viewing.clear();
}
bool ChatMedia::draw(std::string_view markdown, bool headless, const std::function<void(std::string_view)>& text_renderer) {
    const auto text = [&](std::string_view value) {
        if (text_renderer) text_renderer(value);
        else ImGui::TextWrapped("%s", std::string(value).c_str());
    };
    const auto start = markdown.find("!["); if (start == std::string_view::npos) return false;
    const auto split = markdown.find("](", start + 2), end = split == std::string_view::npos ? split : markdown.find(')', split + 2);
    if (end == std::string_view::npos) return false;
    const std::string link(markdown.substr(split + 2, end - split - 2));
    const auto filename = resolve(link);
    const auto extension = filename.extension().string();
    const bool video = extension == ".webm" || extension == ".mp4" || extension == ".mov";
    if (filename.empty() || (!video && extension != ".png" && extension != ".jpg" && extension != ".jpeg")) { ImGui::TextWrapped("Media unavailable: %s", link.c_str()); return true; }
    impl_->collect(headless);
    auto found = impl_->entries.find(link);
    if (found == impl_->entries.end()) {
        if (impl_->entries.size() >= 16) {
            auto oldest = impl_->entries.end();
            for (auto it = impl_->entries.begin(); it != impl_->entries.end(); ++it)
                if (it->first != impl_->viewing && !it->second.pending.valid() && it->second.used_frame < ImGui::GetFrameCount() - 1 && (oldest == impl_->entries.end() || it->second.used_frame < oldest->second.used_frame)) oldest = it;
            if (oldest == impl_->entries.end()) { ImGui::TextDisabled("Media preview limit reached."); return true; }
            impl_->retire(std::move(oldest->second.texture)); impl_->entries.erase(oldest);
        }
        if (std::count_if(impl_->entries.begin(), impl_->entries.end(), [](const auto& item) { return item.second.pending.valid(); }) >= 2) { ImGui::TextDisabled("Loading media..."); return true; }
        auto& entry = impl_->entries[link]; entry.path = filename; entry.video = video;
        entry.pending = std::async(std::launch::async, decode, filename, 0.0, video, 1024U); found = impl_->entries.find(link);
    }
    auto& entry = found->second; entry.used_frame = ImGui::GetFrameCount();
    impl_->update(entry);
    if (entry.large && impl_->viewing != link && !entry.pending.valid()) {
        entry.large = false;
        entry.pending = std::async(std::launch::async, decode, entry.path, 0.0, false, 1024U);
    }
    ImGui::PushID(link.c_str());
    if (start) text(markdown.substr(0, start));
    if (entry.texture) {
        if (headless) { entry.texture->SetTexID(1); entry.texture->SetStatus(ImTextureStatus_OK); }
        else if (entry.texture->Status == ImTextureStatus_Destroyed) entry.texture->SetStatus(ImTextureStatus_WantCreate);
        const float width = std::min(ImGui::GetContentRegionAvail().x, 640.0F);
        ImGui::Image(entry.texture->GetTexRef(), ImVec2(width, width * static_cast<float>(entry.texture->Height) / static_cast<float>(entry.texture->Width)));
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && impl_->viewing.empty()) {
                impl_->viewing = link; impl_->open_pending = true; impl_->zoom = 1; impl_->pan = {};
                if (!entry.video && !entry.pending.valid() && std::max(entry.texture->Width, entry.texture->Height) >= 1024) {
                    entry.large = true;
                    entry.pending = std::async(std::launch::async, decode, entry.path, 0.0, false, 4096U);
                }
            }
        }
    } else ImGui::TextDisabled("%s", entry.error.empty() ? "Loading media..." : entry.error.c_str());
    const std::string caption(markdown.substr(start + 2, split - start - 2));
    if (!caption.empty()) text(caption);
    if (video && entry.texture && impl_->viewing != link) impl_->controls(entry);

    if (end + 1 < markdown.size() && !draw(markdown.substr(end + 1), headless, text_renderer)) text(markdown.substr(end + 1));
    ImGui::PopID(); return true;
}
bool ChatMedia::viewer_open() const { return !impl_->viewing.empty(); }
void ChatMedia::draw_viewer(bool headless) {
    impl_->collect(headless);
    for (auto& [link, cached] : impl_->entries) {
        if (cached.large && link != impl_->viewing && cached.pending.valid()) {
            if (cached.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            (void)cached.pending.get();
        } else impl_->update(cached);
        if (cached.large && link != impl_->viewing && !cached.pending.valid()) {
            cached.large = false;
            cached.pending = std::async(std::launch::async, decode, cached.path, 0.0, false, 1024U);
        }
    }
    if (impl_->viewing.empty()) return;
    const auto found = impl_->entries.find(impl_->viewing);
    if (found == impl_->entries.end() || !found->second.texture) { impl_->viewing.clear(); return; }
    auto& entry = found->second;
    impl_->update(entry);
    entry.used_frame = ImGui::GetFrameCount();
    const auto* viewport = ImGui::GetMainViewport();
    if (impl_->open_pending) { ImGui::OpenPopup("Chat media viewer"); impl_->open_pending = false; }
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, .85F));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));
    if (ImGui::BeginPopupModal("Chat media viewer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // A single canvas captures input, including clicks on the dimmed background.
        const auto origin = ImGui::GetCursorScreenPos();
        const ImVec2 canvas(viewport->Size.x, std::max(1.0F, viewport->Size.y - 64));
        ImGui::InvisibleButton("##MediaCanvas", canvas, ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered();
        auto& io = ImGui::GetIO();
        const float fit = std::min(std::max(1.0F, canvas.x - 48) / static_cast<float>(entry.texture->Width), std::max(1.0F, canvas.y - 48) / static_cast<float>(entry.texture->Height));
        const ImVec2 center(origin.x + canvas.x * .5F, origin.y + canvas.y * .5F);
        if (!entry.video && hovered && io.MouseWheel != 0) {
            const float previous = impl_->zoom;
            impl_->zoom = std::clamp(previous * std::pow(1.2F, io.MouseWheel), 1.0F, 16.0F);
            const float ratio = impl_->zoom / previous;
            impl_->pan.x = (impl_->pan.x + center.x - io.MousePos.x) * ratio + io.MousePos.x - center.x;
            impl_->pan.y = (impl_->pan.y + center.y - io.MousePos.y) * ratio + io.MousePos.y - center.y;
            if (impl_->zoom == 1) impl_->pan = {};
        }
        const ImVec2 size(static_cast<float>(entry.texture->Width) * fit * impl_->zoom, static_cast<float>(entry.texture->Height) * fit * impl_->zoom);
        const ImVec2 minimum(center.x + impl_->pan.x - size.x * .5F, center.y + impl_->pan.y - size.y * .5F);
        const ImVec2 maximum(minimum.x + size.x, minimum.y + size.y);
        const bool on_media = io.MousePos.x >= minimum.x && io.MousePos.x <= maximum.x && io.MousePos.y >= minimum.y && io.MousePos.y <= maximum.y;
        if (!entry.video && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            impl_->pan.x += io.MouseDelta.x; impl_->pan.y += io.MouseDelta.y;
        }
        if (headless) { entry.texture->SetTexID(1); entry.texture->SetStatus(ImTextureStatus_OK); }
        else if (entry.texture->Status == ImTextureStatus_Destroyed) entry.texture->SetStatus(ImTextureStatus_WantCreate);
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(origin, ImVec2(origin.x + canvas.x, origin.y + canvas.y), true);
        draw->AddImage(entry.texture->GetTexRef(), minimum, maximum); draw->PopClipRect();
        if (hovered && on_media && !entry.video) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        ImGui::SetCursorPos(ImVec2(24, canvas.y + 8));
        if (entry.video) impl_->controls(entry);
        else ImGui::TextDisabled("Scroll to zoom  /  Drag to pan  /  Escape or click the background to close");
        const bool footer_background = io.MousePos.y >= origin.y + canvas.y && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
        const bool close = ImGui::IsKeyPressed(ImGuiKey_Escape) || (((hovered && !on_media) || footer_background) && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
        if (close) { ImGui::CloseCurrentPopup(); impl_->viewing.clear(); }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2); ImGui::PopStyleVar(3);
}

}
