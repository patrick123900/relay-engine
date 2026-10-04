#include "relay/editor/interface_preview.hpp"

#include "relay/editor/editor_widgets.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace relay {
namespace {

// ImGui colors and textures are linear values on the sRGB swapchain; interface colors are sRGB.
const std::array<std::uint8_t, 256>& linear_bytes() {
    static const auto table = [] {
        std::array<std::uint8_t, 256> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const float c = static_cast<float>(index) / 255.0F;
            const float linear = c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
            values[index] = static_cast<std::uint8_t>(std::lround(linear * 255.0F));
        }
        return values;
    }();
    return table;
}

ImU32 linear_color(std::uint32_t color) {
    const auto& table = linear_bytes();
    return static_cast<ImU32>(table[color & 0xFFU]) | (static_cast<ImU32>(table[(color >> 8U) & 0xFFU]) << 8U) |
           (static_cast<ImU32>(table[(color >> 16U) & 0xFFU]) << 16U) | (color & 0xFF000000U);
}

} // namespace

InterfacePreview::~InterfacePreview() { clear(); }

void InterfacePreview::retire(std::unique_ptr<ImTextureData> texture) {
    if (!texture) return;
    if (headless_ || texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_Destroyed) {
        ImGui::UnregisterUserTexture(texture.get());
        return;
    }
    texture->SetStatus(ImTextureStatus_WantDestroy);
    retired_.push_back(std::move(texture));
}

void InterfacePreview::begin_frame(const bool headless) {
    headless_ = headless;
    for (auto item = retired_.begin(); item != retired_.end();) {
        if ((*item)->Status == ImTextureStatus_Destroyed) {
            ImGui::UnregisterUserTexture(item->get());
            item = retired_.erase(item);
        } else {
            ++item;
        }
    }
    // Textures the preview stopped drawing go after a while.
    for (auto item = textures_.begin(); item != textures_.end();) {
        if (++item->second.unused > 600) {
            retire(std::move(item->second.data));
            item = textures_.erase(item);
        } else {
            ++item;
        }
    }
}

void InterfacePreview::release() {
    if (!ImGui::GetCurrentContext()) return;
    for (auto& [id, texture] : textures_) retire(std::move(texture.data));
    textures_.clear();
}

void InterfacePreview::clear() {
    if (!ImGui::GetCurrentContext()) return;
    for (auto& [id, texture] : textures_) retire(std::move(texture.data));
    textures_.clear();
    for (auto& texture : retired_) ImGui::UnregisterUserTexture(texture.get());
    retired_.clear();
}

ImTextureData* InterfacePreview::texture(const UiTexture& source) {
    auto& entry = textures_[source.id];
    entry.unused = 0;
    const bool fits = entry.data && entry.data->Width == static_cast<int>(source.width) &&
                      entry.data->Height == static_cast<int>(source.height) &&
                      entry.data->Status != ImTextureStatus_WantDestroy;
    if (fits && entry.revision == source.revision) {
        if (entry.data->Status == ImTextureStatus_Destroyed) entry.data->SetStatus(ImTextureStatus_WantCreate);
        return entry.data.get();
    }
    std::vector<std::uint8_t> pixels = source.rgba;
    srgb_rows_to_linear(pixels);
    entry.revision = source.revision;
    if (fits && entry.data->Status == ImTextureStatus_OK) {
        std::memcpy(entry.data->Pixels, pixels.data(), pixels.size());
        const ImTextureRect whole{0, 0, static_cast<unsigned short>(source.width),
                                  static_cast<unsigned short>(source.height)};
        entry.data->UpdateRect = whole;
        entry.data->Updates.resize(0);
        entry.data->Updates.push_back(whole);
        entry.data->SetStatus(ImTextureStatus_WantUpdates);
        return entry.data.get();
    }
    retire(std::move(entry.data));
    entry.data = std::make_unique<ImTextureData>();
    entry.data->Create(ImTextureFormat_RGBA32, static_cast<int>(source.width), static_cast<int>(source.height));
    std::memcpy(entry.data->Pixels, pixels.data(), pixels.size());
    entry.data->RefCount = 1;
    entry.data->SetStatus(ImTextureStatus_WantCreate);
    ImGui::RegisterUserTexture(entry.data.get());
    if (headless_) {
        entry.data->SetTexID(1);
        entry.data->SetStatus(ImTextureStatus_OK);
    }
    return entry.data.get();
}

void InterfacePreview::draw(ImDrawList* target, const UiDrawList& list, const ImVec2 origin, const float scale,
                            const ImVec2 clip_min, const ImVec2 clip_max) {
    for (const auto& command : list.commands) {
        if (command.texture >= list.textures.size()) continue;
        auto* image = texture(*list.textures[command.texture]);
        const ImVec2 low{std::max(clip_min.x, origin.x + command.clip.x0 * scale),
                         std::max(clip_min.y, origin.y + command.clip.y0 * scale)};
        const ImVec2 high{std::min(clip_max.x, origin.x + command.clip.x1 * scale),
                          std::min(clip_max.y, origin.y + command.clip.y1 * scale)};
        if (high.x <= low.x || high.y <= low.y) continue;
        target->PushClipRect(low, high, true);
        target->PushTexture(image->GetTexRef());
        // Unshared vertices keep each batch within ImGui's 16-bit indices.
        for (std::uint32_t start = 0; start < command.index_count; start += 3U * 20000U) {
            const auto count = std::min(command.index_count - start, 3U * 20000U);
            target->PrimReserve(static_cast<int>(count), static_cast<int>(count));
            for (std::uint32_t item = 0; item < count; ++item) {
                const auto& vertex = list.vertices[list.indices[command.first_index + start + item]];
                target->PrimWriteVtx({origin.x + vertex.x * scale, origin.y + vertex.y * scale}, {vertex.u, vertex.v},
                                     linear_color(vertex.color));
                target->PrimWriteIdx(static_cast<ImDrawIdx>(target->_VtxCurrentIdx - 1U));
            }
        }
        target->PopTexture();
        target->PopClipRect();
    }
}

} // namespace relay
