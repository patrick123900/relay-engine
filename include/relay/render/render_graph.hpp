#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

using RenderResourceId = std::uint32_t;

enum class RenderResourceKind { image, buffer };
// The *_load accesses bind an attachment that an earlier pass wrote and keep adding to it, so
// they order after that pass without counting as a second writer.
enum class RenderAccess { sampled, storage_read, storage_write, color_attachment,
                          depth_stencil_attachment, transfer_source, transfer_destination,
                          present, color_attachment_load, depth_stencil_load };

struct RenderResourceDescription {
    std::string name;
    RenderResourceKind kind{RenderResourceKind::image};
    bool imported{false};
};

struct RenderResourceUse {
    RenderResourceId resource{};
    RenderAccess access{RenderAccess::sampled};
};

struct RenderPassDescription {
    std::string name;
    std::vector<RenderResourceUse> resources;
};

struct RenderTransition {
    RenderResourceId resource{};
    std::string before;
    std::string after;
    std::string pass;
};

struct CompiledRenderGraph {
    bool valid{false};
    std::string error;
    std::vector<RenderResourceDescription> resources;
    std::vector<RenderPassDescription> ordered_passes;
    std::vector<RenderTransition> transitions;

    [[nodiscard]] std::string json() const;
};

class RenderGraph {
public:
    [[nodiscard]] RenderResourceId add_resource(std::string name, RenderResourceKind kind,
                                                bool imported = false);
    void add_pass(std::string name, std::vector<RenderResourceUse> resources);
    [[nodiscard]] CompiledRenderGraph compile() const;

private:
    std::vector<RenderResourceDescription> resources_;
    std::vector<RenderPassDescription> passes_;
};

[[nodiscard]] CompiledRenderGraph make_scene_render_graph();
[[nodiscard]] std::string_view to_string(RenderResourceKind kind);
[[nodiscard]] std::string_view to_string(RenderAccess access);

} // namespace relay
