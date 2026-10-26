#include "relay/render/cooked_assets.hpp"

#include "relay/render/asset_manifest.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

static_assert(std::endian::native == std::endian::little, "cooked files are little endian");

namespace relay {
namespace {

constexpr char models_magic[4] = {'R', 'L', 'Y', 'M'};
constexpr char shader_magic[4] = {'R', 'L', 'Y', 'S'};
constexpr std::uint32_t cooked_version = 1;

class Writer {
public:
    void raw(const void* data, const std::size_t size) { out_.append(static_cast<const char*>(data), size); }
    template <typename T> void value(const T v) { raw(&v, sizeof v); }
    void count(const std::size_t n) { value<std::uint64_t>(n); }
    void text(const std::string& s) { count(s.size()); raw(s.data(), s.size()); }
    template <typename T> void array(const std::vector<T>& v) {
        count(v.size());
        raw(v.data(), v.size() * sizeof(T));
    }
    void strings(const std::vector<std::string>& v) {
        count(v.size());
        for (const auto& s : v) text(s);
    }
    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

class Reader {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : at_(bytes.data()), end_(bytes.data() + bytes.size()) {}
    [[nodiscard]] bool ok() const { return ok_; }
    void raw(void* data, const std::size_t size) {
        if (!ok_ || static_cast<std::size_t>(end_ - at_) < size) { ok_ = false; std::memset(data, 0, size); return; }
        std::memcpy(data, at_, size);
        at_ += size;
    }
    template <typename T> T value() { T v{}; raw(&v, sizeof v); return v; }
    // A count that cannot be more than the bytes left allow, so a damaged file cannot ask for gigabytes.
    std::size_t count(const std::size_t element_bytes) {
        const auto n = value<std::uint64_t>();
        if (!ok_ || (element_bytes != 0U && n > static_cast<std::uint64_t>(end_ - at_) / element_bytes)) { ok_ = false; return 0; }
        return static_cast<std::size_t>(n);
    }
    std::string text() {
        const auto n = count(1);
        std::string s(n, '\0');
        raw(s.data(), n);
        return s;
    }
    template <typename T> std::vector<T> array() {
        const auto n = count(sizeof(T));
        std::vector<T> v(n);
        raw(v.data(), n * sizeof(T));
        return v;
    }
    std::vector<std::string> strings() {
        const auto n = count(8);
        std::vector<std::string> v;
        v.reserve(n);
        for (std::size_t i = 0; i < n && ok_; ++i) v.push_back(text());
        return v;
    }

private:
    const std::uint8_t* at_;
    const std::uint8_t* end_;
    bool ok_{true};
};

// ---- Models -------------------------------------------------------------------------------

void write_vec3(Writer& w, const Vec3& v) { w.value(v.x); w.value(v.y); w.value(v.z); }
Vec3 read_vec3(Reader& r) { Vec3 v; v.x = r.value<double>(); v.y = r.value<double>(); v.z = r.value<double>(); return v; }

void write_mesh(Writer& w, const MeshAsset& m) {
    w.text(m.name);
    w.value(m.first_index); w.value(m.index_count); w.value(m.vertex_offset); w.value(m.vertex_count);
    w.count(m.joints.size());
    for (const auto& joint : m.joints) { w.value(joint.node); w.raw(joint.inverse_bind.data(), sizeof joint.inverse_bind); }
    w.count(m.skin.size());
    for (const auto& influences : m.skin) w.array(influences);
    w.count(m.morph_targets.size());
    for (const auto& target : m.morph_targets) {
        w.text(target.name);
        w.value(target.weight);
        w.array(target.deltas);
    }
    w.text(m.label);
    w.text(m.source);
}

MeshAsset read_mesh(Reader& r) {
    MeshAsset m;
    m.name = r.text();
    m.first_index = r.value<std::uint32_t>(); m.index_count = r.value<std::uint32_t>();
    m.vertex_offset = r.value<std::int32_t>(); m.vertex_count = r.value<std::uint32_t>();
    const auto joints = r.count(68);
    for (std::size_t i = 0; i < joints && r.ok(); ++i) {
        SkinJoint joint;
        joint.node = r.value<std::uint32_t>();
        r.raw(joint.inverse_bind.data(), sizeof joint.inverse_bind);
        m.joints.push_back(joint);
    }
    const auto skins = r.count(8);
    for (std::size_t i = 0; i < skins && r.ok(); ++i) m.skin.push_back(r.array<SkinWeight>());
    const auto targets = r.count(8);
    for (std::size_t i = 0; i < targets && r.ok(); ++i) {
        MorphTarget target;
        target.name = r.text();
        target.weight = r.value<double>();
        target.deltas = r.array<MeshVertex>();
        m.morph_targets.push_back(std::move(target));
    }
    m.label = r.text();
    m.source = r.text();
    return m;
}

void write_material(Writer& w, const MaterialAsset& m) {
    w.text(m.name);
    w.raw(m.color.data(), sizeof m.color);
    w.text(m.texture);
    w.value(m.metallic_factor); w.value(m.roughness_factor);
    w.text(m.metallic_roughness_texture);
    w.text(m.normal_texture); w.value(m.normal_scale);
    w.text(m.occlusion_texture); w.value(m.occlusion_strength);
    w.raw(m.emissive_factor.data(), sizeof m.emissive_factor);
    w.text(m.emissive_texture);
    w.value(static_cast<std::uint8_t>(m.alpha_mode));
    w.value(m.alpha_cutoff);
    w.value<std::uint8_t>(m.double_sided ? 1U : 0U);
    w.text(m.label); w.text(m.source);
}

MaterialAsset read_material(Reader& r) {
    MaterialAsset m;
    m.name = r.text();
    r.raw(m.color.data(), sizeof m.color);
    m.texture = r.text();
    m.metallic_factor = r.value<float>(); m.roughness_factor = r.value<float>();
    m.metallic_roughness_texture = r.text();
    m.normal_texture = r.text(); m.normal_scale = r.value<float>();
    m.occlusion_texture = r.text(); m.occlusion_strength = r.value<float>();
    r.raw(m.emissive_factor.data(), sizeof m.emissive_factor);
    m.emissive_texture = r.text();
    m.alpha_mode = static_cast<MaterialAsset::AlphaMode>(std::min<std::uint8_t>(r.value<std::uint8_t>(), 2U));
    m.alpha_cutoff = r.value<float>();
    m.double_sided = r.value<std::uint8_t>() != 0U;
    m.label = r.text(); m.source = r.text();
    return m;
}

void write_texture(Writer& w, const TextureAsset& t) {
    w.text(t.name);
    w.value(t.width); w.value(t.height);
    w.array(t.rgba);
    for (const auto v : {static_cast<std::uint8_t>(t.color_space), static_cast<std::uint8_t>(t.mag_filter),
                         static_cast<std::uint8_t>(t.min_filter), static_cast<std::uint8_t>(t.mip_filter),
                         static_cast<std::uint8_t>(t.wrap_u), static_cast<std::uint8_t>(t.wrap_v)})
        w.value(v);
}

TextureAsset read_texture(Reader& r) {
    TextureAsset t;
    t.name = r.text();
    t.width = r.value<std::uint32_t>(); t.height = r.value<std::uint32_t>();
    t.rgba = r.array<std::uint8_t>();
    const auto small = [&](const std::uint8_t limit) { return std::min<std::uint8_t>(r.value<std::uint8_t>(), limit); };
    t.color_space = static_cast<TextureColorSpace>(small(1));
    t.mag_filter = static_cast<TextureFilter>(small(1));
    t.min_filter = static_cast<TextureFilter>(small(1));
    t.mip_filter = static_cast<TextureFilter>(small(1));
    t.wrap_u = static_cast<TextureWrap>(small(2));
    t.wrap_v = static_cast<TextureWrap>(small(2));
    return t;
}

void write_model(Writer& w, const ModelAsset& m) {
    w.text(m.name);
    w.strings(m.nodes);
    w.array(m.parents);
    w.count(m.rest_transforms.size());
    for (const auto& t : m.rest_transforms) { write_vec3(w, t.position); write_vec3(w, t.rotation_degrees); write_vec3(w, t.scale); }
    w.strings(m.binding_layout);
    w.count(m.clips.size());
    for (const auto& clip : m.clips) {
        w.text(clip.name);
        w.value(clip.duration_seconds);
        w.count(clip.tracks.size());
        for (const auto& track : clip.tracks) {
            w.value(track.node);
            for (const auto* keys : {&track.positions, &track.scales}) {
                w.count(keys->size());
                for (const auto& k : *keys) { w.value(k.time); write_vec3(w, k.value); write_vec3(w, k.in_tangent); write_vec3(w, k.out_tangent); }
            }
            w.count(track.rotations.size());
            for (const auto& k : track.rotations) { w.value(k.time); w.raw(k.value.data(), 32); w.raw(k.in_tangent.data(), 32); w.raw(k.out_tangent.data(), 32); }
            w.value(static_cast<std::uint8_t>(track.position_interpolation));
            w.value(static_cast<std::uint8_t>(track.scale_interpolation));
            w.value(static_cast<std::uint8_t>(track.rotation_interpolation));
        }
        w.count(clip.morph_tracks.size());
        for (const auto& track : clip.morph_tracks) {
            w.value(track.mesh);
            w.count(track.keys.size());
            for (const auto& k : track.keys) { w.value(k.time); w.array(k.weights); w.array(k.in_tangent); w.array(k.out_tangent); }
            w.value(static_cast<std::uint8_t>(track.interpolation));
        }
    }
}

ModelAsset read_model(Reader& r) {
    ModelAsset m;
    m.name = r.text();
    m.nodes = r.strings();
    m.parents = r.array<std::int32_t>();
    const auto transforms = r.count(72);
    for (std::size_t i = 0; i < transforms && r.ok(); ++i) {
        Transform t;
        t.position = read_vec3(r); t.rotation_degrees = read_vec3(r); t.scale = read_vec3(r);
        m.rest_transforms.push_back(t);
    }
    m.binding_layout = r.strings();
    const auto interpolation = [&] { return static_cast<AnimationInterpolation>(std::min<std::uint8_t>(r.value<std::uint8_t>(), 2U)); };
    const auto clips = r.count(16);
    for (std::size_t c = 0; c < clips && r.ok(); ++c) {
        AnimationClip clip;
        clip.name = r.text();
        clip.duration_seconds = r.value<double>();
        const auto tracks = r.count(8);
        for (std::size_t t = 0; t < tracks && r.ok(); ++t) {
            NodeTrack track;
            track.node = r.value<std::uint32_t>();
            for (auto* keys : {&track.positions, &track.scales}) {
                const auto n = r.count(80);
                for (std::size_t k = 0; k < n && r.ok(); ++k) {
                    VectorKey key;
                    key.time = r.value<double>(); key.value = read_vec3(r); key.in_tangent = read_vec3(r); key.out_tangent = read_vec3(r);
                    keys->push_back(key);
                }
            }
            const auto rotations = r.count(104);
            for (std::size_t k = 0; k < rotations && r.ok(); ++k) {
                RotationKey key;
                key.time = r.value<double>();
                r.raw(key.value.data(), 32); r.raw(key.in_tangent.data(), 32); r.raw(key.out_tangent.data(), 32);
                track.rotations.push_back(key);
            }
            track.position_interpolation = interpolation();
            track.scale_interpolation = interpolation();
            track.rotation_interpolation = interpolation();
            clip.tracks.push_back(std::move(track));
        }
        const auto morphs = r.count(8);
        for (std::size_t t = 0; t < morphs && r.ok(); ++t) {
            MorphTrack track;
            track.mesh = r.value<std::uint32_t>();
            const auto n = r.count(32);
            for (std::size_t k = 0; k < n && r.ok(); ++k) {
                MorphKey key;
                key.time = r.value<double>(); key.weights = r.array<double>(); key.in_tangent = r.array<double>(); key.out_tangent = r.array<double>();
                track.keys.push_back(std::move(key));
            }
            track.interpolation = interpolation();
            clip.morph_tracks.push_back(std::move(track));
        }
        m.clips.push_back(std::move(clip));
    }
    return m;
}

} // namespace

std::string serialize_cooked_models(const CookedModels& cooked) {
    Writer w;
    w.raw(models_magic, sizeof models_magic);
    w.value(cooked_version);
    w.value(cooked.models);
    w.count(cooked.steps.size());
    for (const auto& step : cooked.steps) {
        if (step.batch) {
            const auto& b = *step.batch;
            w.value<std::uint8_t>(1U);
            w.array(b.vertices);
            w.array(b.indices);
            w.count(b.meshes.size());
            for (const auto& mesh : b.meshes) write_mesh(w, mesh);
            w.count(b.materials.size());
            for (const auto& material : b.materials) write_material(w, material);
            w.count(b.textures.size());
            for (const auto& texture : b.textures) write_texture(w, texture);
        } else {
            w.value<std::uint8_t>(2U);
            write_model(w, *step.model);
        }
    }
    return w.take();
}

bool parse_cooked_models(const std::span<const std::uint8_t> bytes, CookedModels& cooked, std::string& error) {
    Reader r(bytes);
    char magic[4]{};
    r.raw(magic, sizeof magic);
    const auto version = r.value<std::uint32_t>();
    if (!r.ok() || std::memcmp(magic, models_magic, sizeof magic) != 0 || version != cooked_version) {
        error = "the game's cooked models are not in a format this Relay reads";
        return false;
    }
    cooked.models = r.value<std::uint32_t>();
    const auto steps = r.count(1);
    for (std::size_t i = 0; i < steps && r.ok(); ++i) {
        CookedModels::Step step;
        const auto kind = r.value<std::uint8_t>();
        if (kind == 1U) {
            ImportBatch b;
            b.vertices = r.array<MeshVertex>();
            b.indices = r.array<std::uint32_t>();
            const auto meshes = r.count(8);
            for (std::size_t m = 0; m < meshes && r.ok(); ++m) b.meshes.push_back(read_mesh(r));
            const auto materials = r.count(8);
            for (std::size_t m = 0; m < materials && r.ok(); ++m) b.materials.push_back(read_material(r));
            const auto textures = r.count(8);
            for (std::size_t m = 0; m < textures && r.ok(); ++m) b.textures.push_back(read_texture(r));
            step.batch = std::move(b);
        } else if (kind == 2U) {
            step.model = read_model(r);
        } else {
            error = "the game's cooked models are damaged";
            return false;
        }
        cooked.steps.push_back(std::move(step));
    }
    if (!r.ok()) {
        error = "the game's cooked models are damaged";
        return false;
    }
    error.clear();
    return true;
}

void apply_cooked_models(const CookedModels& cooked, AssetRegistry& registry) {
    for (const auto& step : cooked.steps) {
        if (step.batch) {
            const auto& b = *step.batch;
            (void)registry.register_imported(b.vertices, b.indices, b.meshes, b.materials, b.textures);
        } else if (step.model) {
            (void)registry.register_model(*step.model);
        }
    }
}

bool cook_models(const std::filesystem::path& root, const std::set<std::string>& sources, std::string& bytes,
                 std::string& error) {
    AssetRegistry registry;
    CookedModels cooked;
    AssetRecorder recorder;
    recorder.batch = [&](const ImportBatch& batch) { cooked.steps.push_back({batch, std::nullopt}); };
    recorder.model = [&](const ModelAsset& model) { cooked.steps.push_back({std::nullopt, model}); };
    registry.set_recorder(std::move(recorder));
    const auto report = reload_imported_assets(root, registry, &sources);
    if (report.failed != 0U || report.changed != 0U) {
        error = report.messages.empty() ? "a model could not be imported" : report.messages.front();
        if (report.messages.size() > 1U) error += " (and " + std::to_string(report.messages.size() - 1U) + " more)";
        return false;
    }
    cooked.models = static_cast<std::uint32_t>(report.restored);
    bytes = serialize_cooked_models(cooked);
    error.clear();
    return true;
}

// ---- Shaders ------------------------------------------------------------------------------

std::string serialize_cooked_shader(const CompiledShader& shader) {
    Writer w;
    w.raw(shader_magic, sizeof shader_magic);
    w.value(cooked_version);
    w.text(shader.path);
    const auto& p = shader.parsed;
    w.value(static_cast<std::uint8_t>(p.type));
    w.value<std::uint8_t>(p.transparent ? 1U : 0U);
    w.value<std::uint8_t>(p.unshaded ? 1U : 0U);
    w.value<std::uint8_t>(p.has_vertex ? 1U : 0U);
    w.value<std::uint8_t>(p.has_fragment ? 1U : 0U);
    w.value<std::uint8_t>(p.reads_blurred ? 1U : 0U);
    w.value(p.uniform_bytes);
    w.value(p.texture_count);
    w.count(p.uniforms.size());
    for (const auto& u : p.uniforms) {
        w.text(u.name);
        w.value(static_cast<std::uint8_t>(u.type));
        w.value(static_cast<std::uint8_t>(u.hint));
        w.raw(u.default_value.data(), sizeof u.default_value);
        w.value(u.minimum); w.value(u.maximum); w.value(u.step);
        w.value(u.offset); w.value(u.line);
    }
    w.array(shader.vertex);
    w.array(shader.fragment);
    w.array(shader.shadow_fragment);
    w.array(shader.hit);
    return w.take();
}

std::shared_ptr<const CompiledShader> parse_cooked_shader(const std::span<const std::uint8_t> bytes,
                                                          std::string& error) {
    Reader r(bytes);
    char magic[4]{};
    r.raw(magic, sizeof magic);
    const auto version = r.value<std::uint32_t>();
    if (!r.ok() || std::memcmp(magic, shader_magic, sizeof magic) != 0 || version != cooked_version) {
        error = "a cooked shader is not in a format this Relay reads";
        return nullptr;
    }
    auto shader = std::make_shared<CompiledShader>();
    shader->path = r.text();
    auto& p = shader->parsed;
    p.type = static_cast<ShaderType>(std::min<std::uint8_t>(r.value<std::uint8_t>(), 1U));
    p.transparent = r.value<std::uint8_t>() != 0U;
    p.unshaded = r.value<std::uint8_t>() != 0U;
    p.has_vertex = r.value<std::uint8_t>() != 0U;
    p.has_fragment = r.value<std::uint8_t>() != 0U;
    p.reads_blurred = r.value<std::uint8_t>() != 0U;
    p.uniform_bytes = r.value<std::uint32_t>();
    p.texture_count = r.value<std::uint32_t>();
    const auto uniforms = r.count(80);
    for (std::size_t i = 0; i < uniforms && r.ok(); ++i) {
        ShaderUniform u;
        u.name = r.text();
        u.type = static_cast<ShaderUniform::Type>(std::min<std::uint8_t>(r.value<std::uint8_t>(), 6U));
        u.hint = static_cast<ShaderUniform::Hint>(std::min<std::uint8_t>(r.value<std::uint8_t>(), 5U));
        r.raw(u.default_value.data(), sizeof u.default_value);
        u.minimum = r.value<double>(); u.maximum = r.value<double>(); u.step = r.value<double>();
        u.offset = r.value<std::uint32_t>(); u.line = r.value<std::uint32_t>();
        p.uniforms.push_back(std::move(u));
    }
    shader->vertex = r.array<std::uint32_t>();
    shader->fragment = r.array<std::uint32_t>();
    shader->shadow_fragment = r.array<std::uint32_t>();
    shader->hit = r.array<std::uint32_t>();
    if (!r.ok() || shader->fragment.empty()) {
        error = "a cooked shader is damaged";
        return nullptr;
    }
    error.clear();
    return shader;
}

bool cook_shader(const std::filesystem::path& root, const std::string_view path, std::string& bytes,
                 std::string& error) {
    std::string read_error;
    const auto text = read_shader_file(root, path, read_error);
    if (!text) {
        error = read_error;
        return false;
    }
    const auto compiled = compile_relay_shader(path, *text);
    if (!compiled || !compiled->ok()) {
        error = std::string{path} + ": " +
                (compiled && !compiled->errors.empty()
                     ? "line " + std::to_string(compiled->errors.front().line) + ": " + compiled->errors.front().text
                     : "it does not compile");
        return false;
    }
    bytes = serialize_cooked_shader(*compiled);
    error.clear();
    return true;
}

} // namespace relay
