#include "relay/render/render_graph.hpp"

#include <algorithm>
#include <deque>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace relay {
namespace {

bool writes(const RenderAccess access) {
    // A depth attachment is bound with a clear load and depth writes enabled, so the pass using it
    // produces it rather than consuming something an earlier pass wrote.
    return access == RenderAccess::storage_write || access == RenderAccess::color_attachment ||
           access == RenderAccess::depth_stencil_attachment ||
           access == RenderAccess::transfer_destination;
}

std::string escape_json(const std::string_view text) {
    std::string output;
    for (const char character : text) {
        if (character == '"' || character == '\\') output += '\\';
        if (character == '\n') output += "\\n";
        else output += character;
    }
    return output;
}

} // namespace

std::string_view to_string(const RenderResourceKind kind) {
    return kind == RenderResourceKind::image ? "image" : "buffer";
}

std::string_view to_string(const RenderAccess access) {
    switch (access) {
    case RenderAccess::sampled: return "sampled";
    case RenderAccess::storage_read: return "storage_read";
    case RenderAccess::storage_write: return "storage_write";
    case RenderAccess::color_attachment: return "color_attachment";
    case RenderAccess::depth_stencil_attachment: return "depth_stencil_attachment";
    case RenderAccess::transfer_source: return "transfer_source";
    case RenderAccess::transfer_destination: return "transfer_destination";
    case RenderAccess::present: return "present";
    }
    return "unknown";
}

RenderResourceId RenderGraph::add_resource(std::string name, const RenderResourceKind kind,
                                           const bool imported) {
    resources_.push_back({std::move(name), kind, imported});
    return static_cast<RenderResourceId>(resources_.size() - 1U);
}

void RenderGraph::add_pass(std::string name, std::vector<RenderResourceUse> resources) {
    passes_.push_back({std::move(name), std::move(resources)});
}

CompiledRenderGraph RenderGraph::compile() const {
    CompiledRenderGraph output;
    output.resources = resources_;
    if (passes_.empty()) {
        output.error = "render graph has no passes";
        return output;
    }
    std::unordered_set<std::string> resource_names;
    for (const auto& resource : resources_) {
        if (resource.name.empty() || !resource_names.insert(resource.name).second) {
            output.error = "render resource names must be non-empty and unique";
            return output;
        }
    }
    std::unordered_set<std::string> pass_names;
    std::vector<std::optional<std::size_t>> writers(resources_.size());
    for (std::size_t pass_index = 0; pass_index < passes_.size(); ++pass_index) {
        const auto& pass = passes_[pass_index];
        if (pass.name.empty() || !pass_names.insert(pass.name).second) {
            output.error = "render pass names must be non-empty and unique";
            return output;
        }
        std::unordered_set<RenderResourceId> used;
        for (const auto& use : pass.resources) {
            if (use.resource >= resources_.size()) {
                output.error = "render pass references an unknown resource";
                return output;
            }
            if (!used.insert(use.resource).second) {
                output.error = "render pass uses the same resource more than once";
                return output;
            }
            if (writes(use.access)) {
                if (writers[use.resource].has_value()) {
                    output.error = "initial render graph permits only one writer per resource";
                    return output;
                }
                writers[use.resource] = pass_index;
            }
        }
    }

    std::vector<std::vector<std::size_t>> edges(passes_.size());
    std::vector<std::size_t> incoming(passes_.size(), 0U);
    for (std::size_t pass_index = 0; pass_index < passes_.size(); ++pass_index) {
        for (const auto& use : passes_[pass_index].resources) {
            if (writes(use.access)) continue;
            const auto writer = writers[use.resource];
            if (!writer.has_value()) {
                if (!resources_[use.resource].imported) {
                    output.error = "render pass reads a resource with no producer";
                    return output;
                }
                continue;
            }
            if (*writer == pass_index) continue;
            edges[*writer].push_back(pass_index);
            ++incoming[pass_index];
        }
    }
    std::deque<std::size_t> ready;
    for (std::size_t index = 0; index < incoming.size(); ++index) {
        if (incoming[index] == 0U) ready.push_back(index);
    }
    std::vector<std::size_t> order;
    while (!ready.empty()) {
        const auto pass = ready.front();
        ready.pop_front();
        order.push_back(pass);
        for (const auto dependent : edges[pass]) {
            if (--incoming[dependent] == 0U) ready.push_back(dependent);
        }
    }
    if (order.size() != passes_.size()) {
        output.error = "render graph contains a dependency cycle";
        return output;
    }

    std::vector<std::string> previous(resources_.size(), "undefined");
    for (const auto pass_index : order) {
        const auto& pass = passes_[pass_index];
        output.ordered_passes.push_back(pass);
        for (const auto& use : pass.resources) {
            const std::string next{to_string(use.access)};
            if (previous[use.resource] != next) {
                output.transitions.push_back({use.resource, previous[use.resource], next, pass.name});
                previous[use.resource] = next;
            }
        }
    }
    output.valid = true;
    return output;
}

std::string CompiledRenderGraph::json() const {
    std::ostringstream output;
    output << "{\"valid\":" << (valid ? "true" : "false") << ",\"error\":\""
           << escape_json(error) << "\",\"resources\":[";
    for (std::size_t index = 0; index < resources.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"id\":" << index << ",\"name\":\"" << escape_json(resources[index].name)
               << "\",\"kind\":\"" << to_string(resources[index].kind)
               << "\",\"imported\":" << (resources[index].imported ? "true" : "false") << '}';
    }
    output << "],\"passes\":[";
    for (std::size_t index = 0; index < ordered_passes.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"name\":\"" << escape_json(ordered_passes[index].name) << "\",\"uses\":[";
        for (std::size_t use_index = 0; use_index < ordered_passes[index].resources.size(); ++use_index) {
            if (use_index != 0U) output << ',';
            const auto& use = ordered_passes[index].resources[use_index];
            output << "{\"resource\":" << use.resource << ",\"access\":\""
                   << to_string(use.access) << "\"}";
        }
        output << "]}";
    }
    output << "],\"transitions\":[";
    for (std::size_t index = 0; index < transitions.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& transition = transitions[index];
        output << "{\"resource\":" << transition.resource << ",\"before\":\""
               << transition.before << "\",\"after\":\"" << transition.after
               << "\",\"pass\":\"" << escape_json(transition.pass) << "\"}";
    }
    output << "]}";
    return output.str();
}

CompiledRenderGraph make_scene_render_graph() {
    RenderGraph graph;
    const auto swapchain = graph.add_resource("swapchain_color", RenderResourceKind::image, true);
    const auto textures = graph.add_resource("bindless_textures", RenderResourceKind::image, true);
    // Depth is created and consumed inside the geometry pass, so unlike the swapchain and the
    // texture table it is not imported from outside the graph.
    const auto depth = graph.add_resource("scene_depth", RenderResourceKind::image);
    const auto shadow = graph.add_resource("directional_shadow", RenderResourceKind::image);
    graph.add_pass("directional_shadow", {{textures, RenderAccess::sampled},
                                           {shadow, RenderAccess::depth_stencil_attachment}});
    graph.add_pass("scene_geometry", {{textures, RenderAccess::sampled},
                                      {shadow, RenderAccess::sampled},
                                      {depth, RenderAccess::depth_stencil_attachment},
                                      {swapchain, RenderAccess::color_attachment}});
    graph.add_pass("present", {{swapchain, RenderAccess::present}});
    return graph.compile();
}

} // namespace relay
