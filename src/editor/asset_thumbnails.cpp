#include "relay/editor/asset_thumbnails.hpp"

#include "relay/editor/editor_widgets.hpp"
#include "relay/render/asset_thumbnail.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <map>

namespace relay {
namespace {

constexpr std::uint32_t thumbnail_size = 128U;
constexpr std::size_t maximum_jobs = 2U;
constexpr std::size_t maximum_textures = 320U;
// A tile counts as shown while it was drawn within this many frames.
constexpr int shown_frames = 2;
constexpr double recheck_seconds = 1.5;

struct Result {
    bool ok{};
    ThumbnailImage image;
};

enum class Kind : std::uint8_t { image, model, mesh, material };

} // namespace

struct AssetThumbnails::Impl {
    // Entries are keyed by path, or "model#mesh" for meshes; `file` is what is read and watched.
    struct Entry {
        Kind kind{};
        std::string file, mesh, shader;
        std::uint64_t stamp{};
        std::unique_ptr<ImTextureData> texture;
        std::future<Result> pending;
        bool wanted{}, failed{};
        int used{-1000};
        double checked{-1000.0};
    };
    struct Retired {
        std::unique_ptr<ImTextureData> texture;
        int age{};
    };

    std::filesystem::path root;
    std::map<std::string, Entry> entries;
    std::vector<Retired> retired;
    bool headless{};
    int frame{};

    static double now() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The file's modification time and size, folded together; zero when it cannot be read.
    [[nodiscard]] std::uint64_t stamp_of(const std::string& path) const {
        if (path.empty()) return 0U;
        std::error_code failure;
        const auto file = root / path;
        const auto time = std::filesystem::last_write_time(file, failure);
        if (failure) return 0U;
        const auto size = std::filesystem::file_size(file, failure);
        return static_cast<std::uint64_t>(time.time_since_epoch().count()) * 1099511628211ULL ^
               (failure ? 0U : static_cast<std::uint64_t>(size));
    }

    Entry& touch(const std::string& key, const Kind kind, const std::string& file, const std::string& shader = {}) {
        auto& entry = entries[key];
        entry.kind = kind;
        entry.file = file;
        entry.used = frame;
        if (entry.shader != shader) {
            entry.shader = shader;
            entry.checked = -1000.0;
        }
        const double time = now();
        if (!entry.pending.valid() && time - entry.checked > recheck_seconds) {
            entry.checked = time;
            auto stamp = stamp_of(file);
            if (kind == Kind::material) stamp = stamp * 31U + stamp_of(shader);
            if (stamp != entry.stamp) {
                entry.stamp = stamp;
                entry.wanted = true;
                entry.failed = false;
            }
        }
        return entry;
    }

    void retire(std::unique_ptr<ImTextureData> texture) {
        if (!texture) return;
        if (headless || texture->Status == ImTextureStatus_WantCreate) {
            ImGui::UnregisterUserTexture(texture.get());
            return;
        }
        texture->SetStatus(ImTextureStatus_WantDestroy);
        retired.push_back({std::move(texture), 0});
    }

    void show(Entry& entry, const std::uint32_t width, const std::uint32_t height, std::vector<std::uint8_t> rgba) {
        if (width == 0U || height == 0U || rgba.size() != static_cast<std::size_t>(width) * height * 4U) {
            entry.failed = true;
            return;
        }
        srgb_rows_to_linear(rgba);
        retire(std::move(entry.texture));
        entry.texture = std::make_unique<ImTextureData>();
        entry.texture->Create(ImTextureFormat_RGBA32, static_cast<int>(width), static_cast<int>(height));
        std::memcpy(entry.texture->Pixels, rgba.data(), rgba.size());
        entry.texture->RefCount = 1;
        entry.texture->SetStatus(ImTextureStatus_WantCreate);
        ImGui::RegisterUserTexture(entry.texture.get());
        if (headless) {
            entry.texture->SetTexID(1);
            entry.texture->SetStatus(ImTextureStatus_OK);
        }
        entry.failed = false;
    }

    void collect() {
        for (auto item = retired.begin(); item != retired.end();) {
            item->texture->UnusedFrames = ++item->age;
            if (item->texture->Status == ImTextureStatus_Destroyed) {
                ImGui::UnregisterUserTexture(item->texture.get());
                item = retired.erase(item);
            } else {
                ++item;
            }
        }
    }

    void finish_jobs() {
        for (auto& [path, entry] : entries) {
            (void)path;
            if (!entry.pending.valid() ||
                entry.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue;
            auto result = entry.pending.get();
            if (result.ok) show(entry, result.image.width, result.image.height, std::move(result.image.rgba));
            else entry.failed = true;
        }
    }

    void start_jobs() {
        std::size_t running = 0;
        std::vector<std::pair<int, std::string>> waiting;
        for (const auto& [path, entry] : entries) {
            if (entry.pending.valid()) ++running;
            else if (entry.wanted && entry.kind != Kind::material && entry.used >= frame - shown_frames)
                waiting.emplace_back(entry.used, path);
        }
        std::sort(waiting.begin(), waiting.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [used, path] : waiting) {
            (void)used;
            if (running >= maximum_jobs) break;
            auto& entry = entries[path];
            entry.wanted = false;
            const bool model = entry.kind == Kind::model || entry.kind == Kind::mesh;
            entry.pending = std::async(std::launch::async, [folder = root, file = entry.file, mesh = entry.mesh, model] {
                Result result;
                std::string error;
                result.ok = model ? model_thumbnail(folder, file, thumbnail_size, result.image, error, mesh)
                                  : image_thumbnail(folder, file, thumbnail_size, result.image, error);
                return result;
            });
            ++running;
        }
    }

    // Releases the least recently shown textures once there are too many.
    void trim() {
        std::size_t count = 0;
        for (const auto& [path, entry] : entries) {
            (void)path;
            count += entry.texture != nullptr;
        }
        if (count <= maximum_textures) return;
        std::vector<std::pair<int, std::string>> oldest;
        for (const auto& [path, entry] : entries)
            if (entry.texture && !entry.pending.valid() && entry.used < frame - 120) oldest.emplace_back(entry.used, path);
        std::sort(oldest.begin(), oldest.end());
        for (const auto& [used, path] : oldest) {
            (void)used;
            if (count <= maximum_textures) break;
            retire(std::move(entries[path].texture));
            entries.erase(path);
            --count;
        }
    }

    void drop_all() {
        for (auto& [path, entry] : entries) {
            (void)path;
            if (entry.pending.valid()) entry.pending.wait();
            retire(std::move(entry.texture));
        }
        entries.clear();
    }
};

AssetThumbnails::AssetThumbnails() : impl_(std::make_unique<Impl>()) {}

AssetThumbnails::~AssetThumbnails() { clear(); }

void AssetThumbnails::begin_frame(const bool headless) {
    impl_->headless = headless;
    ++impl_->frame;
    impl_->collect();
    impl_->finish_jobs();
    impl_->start_jobs();
    impl_->trim();
}

void AssetThumbnails::set_root(const std::filesystem::path& root) {
    if (root == impl_->root) return;
    impl_->drop_all();
    impl_->root = root;
}

ImTextureData* AssetThumbnails::file(const std::string& path, const Source source) {
    auto& entry = impl_->touch(path, source == Source::model ? Kind::model : Kind::image, path);
    return entry.texture.get();
}

ImTextureData* AssetThumbnails::mesh(const std::string& model, const std::string& mesh) {
    auto& entry = impl_->touch(model + '#' + mesh, Kind::mesh, model);
    entry.mesh = mesh;
    return entry.texture.get();
}

ImTextureData* AssetThumbnails::material(const std::string& path, const std::string& shader) {
    auto& entry = impl_->touch(path, Kind::material, path, shader);
    return entry.texture.get();
}

std::string AssetThumbnails::material_request() const {
    const std::string* best = nullptr;
    int newest = std::numeric_limits<int>::min();
    for (const auto& [path, entry] : impl_->entries)
        if (entry.kind == Kind::material && entry.wanted && entry.used >= impl_->frame - shown_frames &&
            entry.used > newest) {
            newest = entry.used;
            best = &path;
        }
    return best ? *best : std::string{};
}

void AssetThumbnails::store_material(const std::string& path, const std::uint32_t width, const std::uint32_t height,
                                     std::vector<std::uint8_t> rgba) {
    const auto found = impl_->entries.find(path);
    if (found == impl_->entries.end() || found->second.kind != Kind::material) return;
    found->second.wanted = false;
    impl_->show(found->second, width, height, std::move(rgba));
}

bool AssetThumbnails::failed(const std::string& path) const {
    const auto found = impl_->entries.find(path);
    return found != impl_->entries.end() && found->second.failed;
}

void AssetThumbnails::release() { impl_->drop_all(); }

bool AssetThumbnails::idle() const { return impl_->retired.empty(); }

void AssetThumbnails::clear() {
    impl_->drop_all();
    for (auto& old : impl_->retired) ImGui::UnregisterUserTexture(old.texture.get());
    impl_->retired.clear();
}

} // namespace relay
