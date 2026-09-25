#include "relay/observe/profiler.hpp"

#include <algorithm>
#include <cmath>

namespace relay {
namespace {

double elapsed_ms(const std::chrono::steady_clock::time_point start,
                  const std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

Profiler::Profiler() : ring_(capacity) { frame_name_ = intern("Frame"); }

std::uint32_t Profiler::intern(const std::string_view name, const ProfileKind kind) {
    std::scoped_lock lock(names_mutex_);
    const std::string key(name);
    if (const auto found = ids_.find(key); found != ids_.end()) return found->second;
    const auto id = static_cast<std::uint32_t>(names_.size());
    names_.push_back(key);
    kinds_.push_back(kind);
    ids_.emplace(key, id);
    return id;
}

std::string Profiler::name(const std::uint32_t id) const {
    std::scoped_lock lock(names_mutex_);
    return id < names_.size() ? names_[id] : std::string{};
}

ProfileKind Profiler::kind(const std::uint32_t id) const {
    std::scoped_lock lock(names_mutex_);
    return id < kinds_.size() ? kinds_[id] : ProfileKind::work;
}

void Profiler::begin_frame() {
    owner_ = std::this_thread::get_id();
    recording_ = true;
    building_.index = next_index_++;
    building_.engine_frame = 0U;
    building_.game = false;
    building_.milliseconds = 0.0;
    building_.gpu_milliseconds = -1.0;
    building_.nodes.clear();
    building_.gpu_passes.clear();
    ProfileNode root;
    root.name = frame_name_;
    root.calls = 1U;
    building_.nodes.push_back(root);
    open_.clear();
    open_.push_back({0, Clock::now()});
}

bool Profiler::begin_scope(const std::uint32_t name) {
    if (!recording_ || std::this_thread::get_id() != owner_ ||
        building_.nodes.size() >= maximum_nodes)
        return false;
    const auto parent = open_.back().node;
    auto& nodes = building_.nodes;
    std::int32_t node = nodes[static_cast<std::size_t>(parent)].first_child;
    std::int32_t last = -1;
    while (node >= 0 && nodes[static_cast<std::size_t>(node)].name != name) {
        last = node;
        node = nodes[static_cast<std::size_t>(node)].next_sibling;
    }
    if (node < 0) {
        node = static_cast<std::int32_t>(nodes.size());
        ProfileNode created;
        created.name = name;
        created.parent = parent;
        created.depth = static_cast<std::uint16_t>(nodes[static_cast<std::size_t>(parent)].depth + 1U);
        nodes.push_back(created);
        if (last < 0) nodes[static_cast<std::size_t>(parent)].first_child = node;
        else nodes[static_cast<std::size_t>(last)].next_sibling = node;
    }
    ++nodes[static_cast<std::size_t>(node)].calls;
    open_.push_back({node, Clock::now()});
    return true;
}

void Profiler::end_scope() {
    // The frame's own scope closes only in end_frame.
    if (!recording_ || open_.size() <= 1U || std::this_thread::get_id() != owner_) return;
    const auto scope = open_.back();
    open_.pop_back();
    building_.nodes[static_cast<std::size_t>(scope.node)].milliseconds +=
        elapsed_ms(scope.start, Clock::now());
}

void Profiler::end_frame(const std::uint64_t engine_frame, const bool game) {
    if (!recording_ || std::this_thread::get_id() != owner_) return;
    const auto now = Clock::now();
    // Scopes left open, such as by an early return, end with the frame.
    for (auto scope = open_.rbegin(); scope != open_.rend(); ++scope)
        building_.nodes[static_cast<std::size_t>(scope->node)].milliseconds +=
            elapsed_ms(scope->start, now);
    open_.clear();
    recording_ = false;
    building_.milliseconds = building_.nodes.front().milliseconds;
    building_.engine_frame = engine_frame;
    building_.game = game;
    std::scoped_lock lock(frames_mutex_);
    if (paused_) return;
    // Swapping keeps both vectors' storage, so steady recording does not allocate.
    std::swap(ring_[ring_head_], building_);
    ring_head_ = (ring_head_ + 1U) % capacity;
    ring_size_ = std::min(ring_size_ + 1U, capacity);
}

std::uint64_t Profiler::current_frame() const { return recording_ ? building_.index : 0U; }

void Profiler::record_gpu(const std::uint64_t frame, const std::vector<ProfileGpuPass>& passes,
                          const double total_milliseconds) {
    std::scoped_lock lock(frames_mutex_);
    auto* target = const_cast<ProfileFrame*>(find_frame(frame));
    if (target == nullptr) return;
    target->gpu_passes = passes;
    target->gpu_milliseconds = total_milliseconds;
}

void Profiler::set_paused(const bool paused) {
    std::scoped_lock lock(frames_mutex_);
    paused_ = paused;
}

bool Profiler::paused() const {
    std::scoped_lock lock(frames_mutex_);
    return paused_;
}

void Profiler::clear() {
    std::scoped_lock lock(frames_mutex_);
    ring_head_ = 0U;
    ring_size_ = 0U;
}

const ProfileFrame* Profiler::find_frame(const std::uint64_t index) const {
    for (std::size_t offset = 1; offset <= ring_size_; ++offset) {
        const auto& candidate = ring_[(ring_head_ + capacity - offset) % capacity];
        if (candidate.index == index) return &candidate;
        if (candidate.index < index) break;
    }
    return nullptr;
}

std::optional<ProfileFrame> Profiler::frame(const std::uint64_t index) const {
    std::scoped_lock lock(frames_mutex_);
    const auto* found = find_frame(index);
    return found ? std::optional{*found} : std::nullopt;
}

ProfileReport Profiler::report(const ProfileQuery& query) const {
    ProfileReport report;
    report.capacity = capacity;
    std::scoped_lock lock(frames_mutex_);
    report.paused = paused_;

    const auto newest = [&](const std::size_t offset) -> const ProfileFrame& {
        return ring_[(ring_head_ + capacity - offset) % capacity];
    };
    const auto history = std::min(query.history, ring_size_);
    for (std::size_t offset = history; offset >= 1U; --offset) {
        const auto& frame = newest(offset);
        report.history.push_back({frame.index, frame.milliseconds, frame.gpu_milliseconds, frame.game});
    }

    std::vector<const ProfileFrame*> selected;
    if (query.frame != 0U) {
        if (const auto* frame = find_frame(query.frame)) selected.push_back(frame);
    } else {
        const auto wanted = std::clamp<std::size_t>(query.frames, 1U, capacity);
        for (std::size_t offset = 1; offset <= ring_size_ && selected.size() < wanted; ++offset) {
            const auto& frame = newest(offset);
            if (!query.game_only || frame.game) selected.push_back(&frame);
        }
        std::reverse(selected.begin(), selected.end());
    }
    report.frames = selected.size();
    if (selected.empty()) return report;
    report.first_frame = selected.front()->index;
    report.last_frame = selected.back()->index;

    std::vector<std::string> names;
    std::vector<ProfileKind> kinds;
    {
        std::scoped_lock name_lock(names_mutex_);
        names = names_;
        kinds = kinds_;
    }

    // Frame times.
    std::vector<double> times;
    times.reserve(selected.size());
    double gpu_total = 0.0;
    std::size_t gpu_frames = 0;
    for (const auto* frame : selected) {
        times.push_back(frame->milliseconds);
        if (frame->gpu_milliseconds >= 0.0) {
            gpu_total += frame->gpu_milliseconds;
            ++gpu_frames;
        }
    }
    const auto count = static_cast<double>(selected.size());
    double sum = 0.0;
    for (const auto time : times) sum += time;
    report.average_ms = sum / count;
    report.minimum_ms = *std::min_element(times.begin(), times.end());
    report.maximum_ms = *std::max_element(times.begin(), times.end());
    std::sort(times.begin(), times.end());
    report.p95_ms = times[std::min(times.size() - 1U,
                                   static_cast<std::size_t>(std::ceil(0.95 * count)) - 1U)];
    report.fps = report.average_ms > 0.0 ? 1000.0 / report.average_ms : 0.0;
    if (gpu_frames != 0U) report.gpu_ms = gpu_total / static_cast<double>(gpu_frames);

    // Merge every selected frame's tree by call path.
    struct Merged {
        std::uint32_t name{};
        std::int32_t parent{-1};
        std::uint16_t depth{};
        double total{};
        double calls{};
        double max{};
        std::vector<std::int32_t> children;
    };
    std::vector<Merged> merged;
    std::unordered_map<std::uint64_t, std::int32_t> by_path;
    std::vector<std::int32_t> mapping;
    for (const auto* frame : selected) {
        mapping.assign(frame->nodes.size(), -1);
        for (std::size_t index = 0; index < frame->nodes.size(); ++index) {
            const auto& node = frame->nodes[index];
            const auto parent = node.parent < 0 ? -1 : mapping[static_cast<std::size_t>(node.parent)];
            const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(parent + 1)) << 32U) |
                             node.name;
            auto [found, inserted] = by_path.try_emplace(key, static_cast<std::int32_t>(merged.size()));
            if (inserted) {
                Merged entry;
                entry.name = node.name;
                entry.parent = parent;
                entry.depth = node.depth;
                merged.push_back(entry);
                if (parent >= 0) merged[static_cast<std::size_t>(parent)].children.push_back(found->second);
            }
            auto& entry = merged[static_cast<std::size_t>(found->second)];
            entry.total += node.milliseconds;
            entry.calls += node.calls;
            entry.max = std::max(entry.max, node.milliseconds);
            mapping[index] = found->second;
        }
    }
    const auto name_of = [&](const std::uint32_t id) {
        return id < names.size() ? names[id] : std::string{};
    };
    const auto kind_of = [&](const std::uint32_t id) {
        return id < kinds.size() ? kinds[id] : ProfileKind::work;
    };
    const auto self_of = [&](const Merged& entry) {
        double children = 0.0;
        for (const auto child : entry.children) children += merged[static_cast<std::size_t>(child)].total;
        return std::max(0.0, entry.total - children);
    };

    constexpr std::size_t maximum_scopes = 512;
    const auto emit = [&](auto&& self, const std::int32_t index, const std::int32_t parent) -> void {
        if (report.scopes.size() >= maximum_scopes) return;
        auto& entry = merged[static_cast<std::size_t>(index)];
        const auto position = static_cast<std::int32_t>(report.scopes.size());
        report.scopes.push_back({name_of(entry.name), parent, entry.depth, kind_of(entry.name),
                                 entry.total / count, self_of(entry) / count, entry.calls / count,
                                 entry.max});
        std::sort(entry.children.begin(), entry.children.end(), [&](const auto a, const auto b) {
            return merged[static_cast<std::size_t>(a)].total > merged[static_cast<std::size_t>(b)].total;
        });
        for (const auto child : entry.children) self(self, child, position);
    };
    if (!merged.empty()) emit(emit, 0, -1);

    // Hotspots merge a name's time across call paths. The frame's own self time is the part of
    // the frame no scope measured.
    std::unordered_map<std::string, std::size_t> hotspot_index;
    for (std::size_t index = 0; index < merged.size(); ++index) {
        const auto& entry = merged[index];
        if (kind_of(entry.name) == ProfileKind::wait) report.wait_ms += entry.total / count;
        const auto label = index == 0U ? std::string("Untracked") : name_of(entry.name);
        auto [found, inserted] = hotspot_index.try_emplace(label, report.hotspots.size());
        if (inserted) report.hotspots.push_back({label, kind_of(entry.name), 0.0, 0.0, 0.0});
        auto& hotspot = report.hotspots[found->second];
        hotspot.self_ms += self_of(entry) / count;
        hotspot.total_ms += index == 0U ? self_of(entry) / count : entry.total / count;
        hotspot.calls += index == 0U ? 0.0 : entry.calls / count;
    }
    std::stable_sort(report.hotspots.begin(), report.hotspots.end(),
                     [](const auto& a, const auto& b) { return a.self_ms > b.self_ms; });
    report.cpu_ms = std::max(0.0, report.average_ms - report.wait_ms);

    // GPU passes, averaged over the frames whose timings arrived.
    std::unordered_map<std::uint32_t, std::size_t> pass_index;
    for (const auto* frame : selected) {
        for (const auto& pass : frame->gpu_passes) {
            auto [found, inserted] = pass_index.try_emplace(pass.name, report.gpu_passes.size());
            if (inserted) report.gpu_passes.push_back({name_of(pass.name), 0.0, 0.0});
            auto& target = report.gpu_passes[found->second];
            target.average_ms += pass.milliseconds;
            target.max_ms = std::max(target.max_ms, pass.milliseconds);
        }
    }
    for (auto& pass : report.gpu_passes)
        pass.average_ms /= static_cast<double>(std::max<std::size_t>(gpu_frames, 1U));

    // Whichever side is busy for most of the frame limits it. When neither is, the frame is
    // waiting on presentation or pacing.
    if (report.gpu_ms >= 0.0 && report.gpu_ms >= 0.8 * report.average_ms) report.bottleneck = "gpu";
    else if (report.cpu_ms >= 0.8 * report.average_ms) report.bottleneck = "cpu";
    else report.bottleneck = "display";
    return report;
}

Profiler& profiler() {
    static Profiler instance;
    return instance;
}

} // namespace relay
