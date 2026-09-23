#include "relay/physics/collision.hpp"
#include "relay/editor/editor_math.hpp"

#include "collider_mesh.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <limits>

namespace relay {
namespace {

namespace Layers {
constexpr JPH::ObjectLayer stationary = 0;
constexpr JPH::ObjectLayer moving = 1;
}
namespace BroadLayers {
const JPH::BroadPhaseLayer stationary{0};
const JPH::BroadPhaseLayer moving{1};
}

void initialize_jolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

class BroadLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == Layers::moving ? BroadLayers::moving : BroadLayers::stationary;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadLayers::moving ? "moving" : "stationary";
    }
#endif
};

class BroadFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        return layer == Layers::moving || broad == BroadLayers::moving;
    }
};

class PairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override {
        return first == Layers::moving || second == Layers::moving;
    }
};

Entity from_user_data(const JPH::uint64 value) {
    return {static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32U)};
}

bool related(const Scene& scene, Entity first, Entity second) {
    for (auto current = first; current.valid();) {
        if (current == second) return true;
        const auto* record = scene.get(current);
        current = record ? record->parent : Entity{};
    }
    for (auto current = second; current.valid();) {
        if (current == first) return true;
        const auto* record = scene.get(current);
        current = record ? record->parent : Entity{};
    }
    return false;
}

class ContactFilter final : public JPH::ContactListener {
public:
    const Scene* scene{};
    std::map<JPH::BodyID, Entity> entities;
    std::map<std::pair<Entity, Entity>, unsigned> active_pairs;
    std::vector<ContactEvent> pending;
    JPH::ValidateResult OnContactValidate(const JPH::Body& first, const JPH::Body& second,
                                           JPH::RVec3Arg,
                                           const JPH::CollideShapeResult&) override {
        if (!scene) return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        const auto first_entity = from_user_data(first.GetUserData());
        const auto second_entity = from_user_data(second.GetUserData());
        const auto* a = scene->get(first_entity);
        const auto* b = scene->get(second_entity);
        if (!a || !b || !a->collider || !b->collider ||
            !(a->collider->layer & b->collider->mask) ||
            !(b->collider->layer & a->collider->mask) ||
            related(*scene, first_entity, second_entity))
            return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }
    void OnContactAdded(const JPH::Body& first, const JPH::Body& second,
                        const JPH::ContactManifold&, JPH::ContactSettings&) override {
        const auto first_entity = from_user_data(first.GetUserData());
        const auto second_entity = from_user_data(second.GetUserData());
        auto pair = std::minmax(first_entity, second_entity);
        auto& count = active_pairs[{pair.first, pair.second}];
        if (count++ == 0) pending.push_back({0, pair.first, pair.second, true});
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& shapes) override {
        const auto first = entities.find(shapes.GetBody1ID());
        const auto second = entities.find(shapes.GetBody2ID());
        if (first == entities.end() || second == entities.end()) return;
        auto pair = std::minmax(first->second, second->second);
        const auto found = active_pairs.find({pair.first, pair.second});
        if (found != active_pairs.end() && --found->second == 0) {
            pending.push_back({0, pair.first, pair.second, false});
            active_pairs.erase(found);
        }
    }
};

JPH::Vec3 to_jolt(const Vec3 value) {
    return {static_cast<float>(value.x), static_cast<float>(value.y),
            static_cast<float>(value.z)};
}
JPH::RVec3 to_jolt_position(const Vec3 value) {
    return {static_cast<JPH::Real>(value.x), static_cast<JPH::Real>(value.y),
            static_cast<JPH::Real>(value.z)};
}
Vec3 from_jolt(const JPH::Vec3 value) {
    return {value.GetX(), value.GetY(), value.GetZ()};
}
#ifdef JPH_DOUBLE_PRECISION
Vec3 from_jolt(const JPH::RVec3 value) {
    return {value.GetX(), value.GetY(), value.GetZ()};
}
#endif

JPH::Quat rotation_from_euler(const Vec3 degrees) {
    constexpr double radians = 3.14159265358979323846 / 180.0;
    return JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), static_cast<float>(degrees.z * radians)) *
           JPH::Quat::sRotation(JPH::Vec3::sAxisY(), static_cast<float>(degrees.y * radians)) *
           JPH::Quat::sRotation(JPH::Vec3::sAxisX(), static_cast<float>(degrees.x * radians));
}

Vec3 euler_from_rotation(const JPH::Quat rotation) {
    const auto x = rotation * JPH::Vec3::sAxisX();
    const auto y = rotation * JPH::Vec3::sAxisY();
    const auto z = rotation * JPH::Vec3::sAxisZ();
    EditorMatrix matrix = editor_identity();
    matrix[0] = x.GetX(); matrix[1] = x.GetY(); matrix[2] = x.GetZ();
    matrix[4] = y.GetX(); matrix[5] = y.GetY(); matrix[6] = y.GetZ();
    matrix[8] = z.GetX(); matrix[9] = z.GetY(); matrix[10] = z.GetZ();
    Vec3 position{}, angles{}, scale{};
    editor_decompose(matrix, position, angles, scale);
    return angles;
}

struct WorldTransform {
    Vec3 position{};
    std::array<Vec3, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    JPH::Quat rotation{JPH::Quat::sIdentity()};
};

Vec3 transform_direction(const std::array<Vec3, 3>& axes, Vec3 value) {
    return {axes[0].x * value.x + axes[1].x * value.y + axes[2].x * value.z,
            axes[0].y * value.x + axes[1].y * value.y + axes[2].y * value.z,
            axes[0].z * value.x + axes[1].z * value.y + axes[2].z * value.z};
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

std::optional<WorldTransform> world_transform(const Scene& scene, Entity entity) {
    std::array<Entity, 4096> chain{};
    std::size_t count{};
    for (auto current = entity; current.valid();) {
        if (count == chain.size() || !scene.contains(current)) return std::nullopt;
        chain[count++] = current;
        current = scene.get(current)->parent;
    }
    WorldTransform world;
    while (count) {
        const auto* record = scene.get(chain[--count]);
        const auto transform = record->transform_animation &&
                                       !record->transform_animation->keys.empty()
            ? sample_transform_animation(*record->transform_animation, record->transform)
            : record->transform;
        const Vec3 translated = transform_direction(world.axes, transform.position);
        world.position = {world.position.x + translated.x, world.position.y + translated.y,
                          world.position.z + translated.z};
        const auto local_rotation = rotation_from_euler(transform.rotation_degrees);
        const std::array<JPH::Vec3, 3> local_axes{{
            local_rotation * JPH::Vec3::sAxisX() * static_cast<float>(transform.scale.x),
            local_rotation * JPH::Vec3::sAxisY() * static_cast<float>(transform.scale.y),
            local_rotation * JPH::Vec3::sAxisZ() * static_cast<float>(transform.scale.z)}};
        const auto parent_axes = world.axes;
        for (std::size_t axis = 0; axis < 3; ++axis)
            world.axes[axis] = transform_direction(parent_axes, from_jolt(local_axes[axis]));
        world.rotation = world.rotation * local_rotation;
    }
    return world;
}

std::optional<JPH::ShapeRefC> shape_from_box(const CollisionDebugBox& box,
                                             const WorldTransform& transform) {
    const JPH::Quat inverse = transform.rotation.Conjugated();
    std::array<JPH::Vec3, 8> corners{};
    for (unsigned corner = 0; corner < 8; ++corner) {
        Vec3 point = box.center;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const double sign = corner & (1U << axis) ? 1.0 : -1.0;
            point.x += sign * box.edges[axis].x;
            point.y += sign * box.edges[axis].y;
            point.z += sign * box.edges[axis].z;
        }
        const Vec3 relative{point.x - transform.position.x,
                            point.y - transform.position.y,
                            point.z - transform.position.z};
        corners[corner] = inverse * to_jolt(relative);
    }
    JPH::ConvexHullShapeSettings settings(corners.data(), static_cast<int>(corners.size()), 0.0f);
    settings.SetEmbedded();
    settings.mHullTolerance = 1e-6f;
    const auto result = settings.Create();
    if (result.HasError()) return std::nullopt;
    return result.Get();
}

bool dynamic_body(const EntityRecord& record) {
    return record.physics_body &&
           record.physics_body->type == PhysicsBody::Type::dynamic &&
           (!record.transform_animation || record.transform_animation->keys.empty());
}

std::optional<JPH::ShapeRefC> shape_from_round(const BoxCollider& collider,
                                                const WorldTransform& transform) {
    const auto length = [](Vec3 axis) {
        return std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    };
    const double world_scale = std::max({length(transform.axes[0]),
                                         length(transform.axes[1]),
                                         length(transform.axes[2])});
    const double radius = collider.radius * world_scale;
    const double half_height = collider.half_height * world_scale;
    if (!std::isfinite(radius) || radius <= 0.0 || radius > 1e7 ||
        (collider.type == BoxCollider::Type::capsule &&
         (!std::isfinite(half_height) || half_height <= 0.0 || half_height > 1e7)))
        return std::nullopt;
    JPH::ShapeRefC shape;
    if (collider.type == BoxCollider::Type::sphere) {
        const auto result = JPH::SphereShapeSettings(static_cast<float>(radius)).Create();
        if (result.HasError()) return std::nullopt;
        shape = result.Get();
    } else {
        const auto result = JPH::CapsuleShapeSettings(static_cast<float>(half_height),
                                                       static_cast<float>(radius)).Create();
        if (result.HasError()) return std::nullopt;
        shape = result.Get();
    }
    const auto offset = transform_direction(transform.axes, collider.center);
    const auto local_offset = transform.rotation.Conjugated() * to_jolt(offset);
    const auto translated = JPH::RotatedTranslatedShapeSettings(
        local_offset, JPH::Quat::sIdentity(), shape.GetPtr()).Create();
    if (translated.HasError()) return std::nullopt;
    return translated.Get();
}

std::optional<JPH::ShapeRefC> convex_hull(const std::vector<JPH::Vec3>& points) {
    JPH::ConvexHullShapeSettings settings(points.data(), static_cast<int>(points.size()), 0.0f);
    settings.SetEmbedded();
    const auto result = settings.Create();
    if (result.HasError()) return std::nullopt;
    return result.Get();
}

// Convex hulls and triangle meshes bake the full world scale, including non-uniform and
// hierarchical scale, into body-local vertices. Jolt cannot simulate a dynamic triangle mesh, so a
// dynamic body uses the convex hull of its mesh collider instead.
std::optional<JPH::ShapeRefC> shape_from_mesh(const EntityRecord& record,
                                               const WorldTransform& transform,
                                               const AssetRegistry& assets, bool dynamic,
                                               std::size_t& triangle_budget, bool& over_budget) {
    const auto mesh = detail::collider_mesh(record, assets);
    if (!mesh) return std::nullopt;
    const JPH::Quat inverse = transform.rotation.Conjugated();
    std::vector<JPH::Vec3> points;
    points.reserve(mesh->points.size());
    for (const auto& point : mesh->points) {
        const auto world = transform_direction(transform.axes, point);
        if (std::abs(world.x) > 1e7 || std::abs(world.y) > 1e7 || std::abs(world.z) > 1e7)
            return std::nullopt;
        points.push_back(inverse * to_jolt(world));
    }
    if (record.collider->type == BoxCollider::Type::convex || dynamic) return convex_hull(points);
    if (mesh->triangles.size() > triangle_budget) {
        over_budget = true;
        return std::nullopt;
    }
    const auto& axes = transform.axes;
    const bool mirrored = dot(axes[0], cross(axes[1], axes[2])) < 0.0;
    JPH::VertexList vertices;
    vertices.reserve(points.size());
    for (const auto& point : points) vertices.emplace_back(point.GetX(), point.GetY(), point.GetZ());
    JPH::IndexedTriangleList triangles;
    triangles.reserve(mesh->triangles.size());
    for (const auto& triangle : mesh->triangles)
        triangles.emplace_back(triangle[0], mirrored ? triangle[2] : triangle[1],
                               mirrored ? triangle[1] : triangle[2]);
    JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
    settings.SetEmbedded();
    const auto result = settings.Create();
    if (result.HasError()) return std::nullopt;
    triangle_budget -= mesh->triangles.size();
    return result.Get();
}

bool triangle_mesh(const JPH::Body& body) {
    return body.GetShape()->GetSubType() == JPH::EShapeSubType::Mesh;
}

class JoltState {
public:
    explicit JoltState(const AssetRegistry* assets) : jobs_(JPH::cMaxPhysicsJobs), assets_(assets) {
        system_.Init(4096, 0, 16384, 8192, broad_layers_, broad_filter_, pair_filter_);
        system_.SetContactListener(&contact_filter_);
    }
    ~JoltState() {
        auto& api = system_.GetBodyInterface();
        for (const auto& [entity, id] : bodies_) {
            (void)entity;
            api.RemoveBody(id);
            api.DestroyBody(id);
        }
    }

    void build(const Scene& scene, bool queries_only = false) {
        built_ = true;
        contact_filter_.scene = &scene;
        const auto debug = collision_debug_boxes(scene, true);
        std::map<Entity, CollisionDebugBox> colliders;
        for (const auto& box : debug.boxes) if (box.enabled) colliders.emplace(box.entity, box);
        for (const auto entity : scene.entities()) {
            const auto* record = scene.get(entity);
            const auto found = colliders.find(entity);
            const bool mesh_based = record->collider && record->collider->enabled &&
                                    (record->collider->type == BoxCollider::Type::convex ||
                                     record->collider->type == BoxCollider::Type::mesh);
            const bool has_collider = found != colliders.end() || mesh_based;
            if (!has_collider && (!record->physics_body || queries_only)) continue;
            const auto transform = world_transform(scene, entity);
            if (!transform) continue;
            JPH::ShapeRefC shape;
            if (has_collider) {
                std::optional<JPH::ShapeRefC> created;
                if (mesh_based) {
                    if (assets_)
                        created = shape_from_mesh(*record, *transform, *assets_,
                                                  !queries_only && dynamic_body(*record),
                                                  triangle_budget_, over_budget_);
                } else if (record->collider->type == BoxCollider::Type::box) {
                    created = shape_from_box(found->second, *transform);
                } else {
                    created = shape_from_round(*record->collider, *transform);
                }
                if (!created) continue;
                shape = *created;
            } else {
                // Jolt cannot assign dynamic mass to EmptyShape. The contact listener rejects
                // this proxy because the Relay entity has no enabled collider.
                shape = new JPH::SphereShape(0.1f);
            }
            const bool dynamic = !queries_only && dynamic_body(*record);
            const bool animated = record->transform_animation &&
                                  !record->transform_animation->keys.empty();
            const auto motion = dynamic ? JPH::EMotionType::Dynamic :
                                animated && !queries_only ? JPH::EMotionType::Kinematic :
                                JPH::EMotionType::Static;
            const auto layer = dynamic || motion == JPH::EMotionType::Kinematic
                ? Layers::moving : Layers::stationary;
            JPH::BodyCreationSettings settings(shape.GetPtr(), to_jolt_position(transform->position),
                                                transform->rotation, motion, layer);
            settings.mUserData = entity.packed();
            if (record->physics_body) {
                settings.mGravityFactor = static_cast<float>(record->physics_body->gravity_scale);
                settings.mRestitution = static_cast<float>(record->physics_body->restitution);
                settings.mFriction = static_cast<float>(record->physics_body->friction);
                settings.mLinearDamping = static_cast<float>(record->physics_body->linear_damping);
                settings.mAngularDamping = static_cast<float>(record->physics_body->angular_damping);
                if (dynamic) {
                    settings.mOverrideMassProperties =
                        JPH::EOverrideMassProperties::CalculateInertia;
                    settings.mMassPropertiesOverride.mMass =
                        static_cast<float>(record->physics_body->mass);
                    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
                }
            }
            const auto id = system_.GetBodyInterface().CreateAndAddBody(settings,
                dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
            if (!id.IsInvalid()) {
                bodies_.emplace(entity, id);
                contact_filter_.entities.emplace(id, entity);
            }
        }
        system_.OptimizeBroadPhase();
    }

    void step(Scene& scene, double seconds) {
        if (!built_) build(scene);
        auto& api = system_.GetBodyInterface();
        for (const auto& [entity, id] : bodies_) {
            const auto* record = scene.get(entity);
            if (!record || !record->transform_animation ||
                record->transform_animation->keys.empty()) continue;
            const auto pose = world_transform(scene, entity);
            if (!pose) continue;
            api.MoveKinematic(id,
                to_jolt_position(pose->position),
                pose->rotation, static_cast<float>(seconds));
        }
        system_.Update(static_cast<float>(seconds), 1, &allocator_, &jobs_);
        std::stable_sort(contact_filter_.pending.begin(), contact_filter_.pending.end(),
                         [](const ContactEvent& a, const ContactEvent& b) {
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        });
        for (auto event : contact_filter_.pending) {
            event.sequence = ++latest_sequence_;
            events_.push_back(event);
            if (events_.size() > 1024) events_.pop_front();
        }
        contact_filter_.pending.clear();
        for (const auto& [entity, id] : bodies_) {
            auto* record = scene.get(entity);
            if (!record || !dynamic_body(*record)) continue;
            JPH::RVec3 world_position;
            JPH::Quat world_rotation;
            api.GetPositionAndRotation(id, world_position, world_rotation);
            Vec3 local_position = from_jolt(world_position);
            JPH::Quat local_rotation = world_rotation;
            if (record->parent.valid()) {
                const auto parent = world_transform(scene, record->parent);
                if (!parent) continue;
                const Vec3 relative{local_position.x - parent->position.x,
                                    local_position.y - parent->position.y,
                                    local_position.z - parent->position.z};
                const auto& axes = parent->axes;
                const double determinant = dot(axes[0], cross(axes[1], axes[2]));
                if (std::abs(determinant) < 1e-30) continue;
                local_position = {dot(relative, cross(axes[1], axes[2])) / determinant,
                                  dot(relative, cross(axes[2], axes[0])) / determinant,
                                  dot(relative, cross(axes[0], axes[1])) / determinant};
                local_rotation = parent->rotation.Conjugated() * world_rotation;
            }
            auto authored = record->transform;
            authored.position = local_position;
            authored.rotation_degrees = euler_from_rotation(local_rotation);
            (void)scene.set_transform(entity, authored);
        }
    }

    [[nodiscard]] std::optional<JPH::BodyID> body(Entity entity) const {
        const auto found = bodies_.find(entity);
        return found == bodies_.end() ? std::nullopt : std::optional{found->second};
    }
    JPH::PhysicsSystem& system() { return system_; }
    const JPH::PhysicsSystem& system() const { return system_; }
    [[nodiscard]] bool over_budget() const { return over_budget_; }
    ContactEvents contact_events(std::uint64_t after) const {
        ContactEvents result;
        result.latest_sequence = latest_sequence_;
        result.oldest_sequence = events_.empty() ? latest_sequence_ + 1 : events_.front().sequence;
        for (const auto& event : events_)
            if (event.sequence > after) result.events.push_back(event);
        return result;
    }
private:
    BroadLayerInterface broad_layers_;
    BroadFilter broad_filter_;
    PairFilter pair_filter_;
    ContactFilter contact_filter_;
    JPH::PhysicsSystem system_;
    JPH::TempAllocatorMalloc allocator_;
    JPH::JobSystemSingleThreaded jobs_;
    std::map<Entity, JPH::BodyID> bodies_;
    std::deque<ContactEvent> events_;
    std::uint64_t latest_sequence_{};
    const AssetRegistry* assets_{};
    std::size_t triangle_budget_{detail::maximum_world_collider_triangles};
    bool over_budget_{};
    bool built_{};
};

class ExcludeTriangleMeshes final : public JPH::BodyFilter {
public:
    bool ShouldCollideLocked(const JPH::Body& body) const override { return !triangle_mesh(body); }
};

} // namespace

struct PhysicsWorld::Impl {
    std::unique_ptr<JoltState> state;
    const AssetRegistry* assets{};
};

std::vector<std::array<Vec3, 2>> detail::convex_hull_edges(const std::vector<Vec3>& points,
                                                           std::size_t limit, bool& truncated) {
    std::vector<std::array<Vec3, 2>> result;
    if (points.empty()) return result;
    initialize_jolt();
    // Build relative to the first point so single-precision hull vertices stay accurate.
    const Vec3 origin = points.front();
    std::vector<JPH::Vec3> relative;
    relative.reserve(points.size());
    for (const auto& point : points)
        relative.push_back(to_jolt({point.x - origin.x, point.y - origin.y, point.z - origin.z}));
    const auto shape = convex_hull(relative);
    if (!shape) return result;
    const auto* hull = static_cast<const JPH::ConvexHullShape*>(shape->GetPtr());
    std::set<std::pair<JPH::uint, JPH::uint>> seen;
    std::vector<JPH::uint> face;
    for (JPH::uint index = 0; index < hull->GetNumFaces(); ++index) {
        face.resize(hull->GetNumVerticesInFace(index));
        const auto count = hull->GetFaceVertices(index, static_cast<JPH::uint>(face.size()),
                                                 face.data());
        for (JPH::uint corner = 0; corner < count; ++corner) {
            const auto edge = std::minmax(face[corner], face[(corner + 1U) % count]);
            if (!seen.insert(edge).second) continue;
            if (result.size() == limit) {
                truncated = true;
                return result;
            }
            const auto a = from_jolt(hull->GetPoint(edge.first));
            const auto b = from_jolt(hull->GetPoint(edge.second));
            result.push_back({Vec3{a.x + origin.x, a.y + origin.y, a.z + origin.z},
                              Vec3{b.x + origin.x, b.y + origin.y, b.z + origin.z}});
        }
    }
    return result;
}

PhysicsWorld::PhysicsWorld() : impl_(std::make_unique<Impl>()) {}
PhysicsWorld::~PhysicsWorld() = default;
void PhysicsWorld::reset() { impl_->state.reset(); }
void PhysicsWorld::set_assets(const AssetRegistry* assets) {
    impl_->assets = assets;
    impl_->state.reset();
}
void PhysicsWorld::step(Scene& scene, double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    initialize_jolt();
    if (!impl_->state) impl_->state = std::make_unique<JoltState>(impl_->assets);
    impl_->state->step(scene, seconds);
}
ContactEvents PhysicsWorld::contact_events(std::uint64_t after) const {
    return impl_->state ? impl_->state->contact_events(after) : ContactEvents{{}, 0, 1};
}
std::optional<Vec3> PhysicsWorld::velocity(const Scene& scene, Entity entity) const {
    if (!scene.contains(entity) || !dynamic_body(*scene.get(entity))) return std::nullopt;
    if (!impl_->state) return Vec3{};
    const auto id = impl_->state->body(entity);
    return id ? from_jolt(impl_->state->system().GetBodyInterface().GetLinearVelocity(*id))
              : std::optional<Vec3>{Vec3{}};
}
std::optional<Vec3> PhysicsWorld::angular_velocity(const Scene& scene, Entity entity) const {
    if (!scene.contains(entity) || !dynamic_body(*scene.get(entity))) return std::nullopt;
    if (!impl_->state) return Vec3{};
    const auto id = impl_->state->body(entity);
    return id ? from_jolt(impl_->state->system().GetBodyInterface().GetAngularVelocity(*id))
              : std::optional<Vec3>{Vec3{}};
}
bool PhysicsWorld::apply_impulse(const Scene& scene, Entity entity, Vec3 impulse,
                                 std::optional<Vec3> world_point) {
    if (!scene.contains(entity) || !dynamic_body(*scene.get(entity)) ||
        !std::isfinite(impulse.x) || !std::isfinite(impulse.y) ||
        !std::isfinite(impulse.z) ||
        (world_point && (!std::isfinite(world_point->x) || !std::isfinite(world_point->y) ||
                         !std::isfinite(world_point->z)))) return false;
    initialize_jolt();
    if (!impl_->state) {
        impl_->state = std::make_unique<JoltState>(impl_->assets);
        impl_->state->build(scene);
    }
    const auto id = impl_->state->body(entity);
    if (!id) return false;
    auto& api = impl_->state->system().GetBodyInterface();
    if (world_point) api.AddImpulse(*id, to_jolt(impulse), to_jolt_position(*world_point));
    else api.AddImpulse(*id, to_jolt(impulse));
    api.ActivateBody(*id);
    return true;
}

namespace {
bool query_scene_valid(const Scene& scene, std::string& error) {
    std::size_t active = 0;
    for (const auto entity : scene.entities()) {
        const auto* record = scene.get(entity);
        if (record && record->collider && record->collider->enabled && ++active > 4096U) {
            error = "collision query exceeds 4096 active colliders";
            return false;
        }
    }
    return true;
}
} // namespace

CollisionRaycast collision_raycast(const Scene& scene, const Vec3 origin, Vec3 direction,
                                   double maximum_distance, std::uint32_t layer_mask,
                                   const AssetRegistry* assets) {
    CollisionRaycast result;
    const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                    direction.z * direction.z);
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        !std::isfinite(length) || length == 0.0 || !std::isfinite(maximum_distance) ||
        maximum_distance < 0.0) {
        result.error = "invalid collision ray";
        return result;
    }
    if (!query_scene_valid(scene, result.error)) return result;
    initialize_jolt();
    JoltState state(assets);
    state.build(scene, true);
    if (state.over_budget()) {
        result.error = "collision query exceeds the mesh collider triangle budget";
        return result;
    }
    direction = {direction.x / length, direction.y / length, direction.z / length};
    const JPH::RRayCast ray(to_jolt_position(origin), to_jolt({
        direction.x * maximum_distance, direction.y * maximum_distance,
        direction.z * maximum_distance}));
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> hits;
    state.system().GetNarrowPhaseQuery().CastRay(ray, {}, hits);
    float nearest = std::numeric_limits<float>::infinity();
    for (const auto& hit : hits.mHits) {
        const auto entity = from_user_data(state.system().GetBodyInterface().GetUserData(hit.mBodyID));
        const auto* record = scene.get(entity);
        if (!record || !record->collider || !(record->collider->layer & layer_mask) ||
            hit.mFraction >= nearest) continue;
        JPH::BodyLockRead lock(state.system().GetBodyLockInterface(), hit.mBodyID);
        if (!lock.Succeeded()) continue;
        nearest = hit.mFraction;
        result.hit = true;
        result.entity = entity;
        result.distance = static_cast<double>(nearest) * maximum_distance;
        result.point = {origin.x + direction.x * result.distance,
                        origin.y + direction.y * result.distance,
                        origin.z + direction.z * result.distance};
        result.normal = from_jolt(lock.GetBody().GetWorldSpaceSurfaceNormal(
            hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction)));
    }
    return result;
}

CollisionOverlaps collision_overlaps(const Scene& scene, Entity entity,
                                    const AssetRegistry* assets) {
    CollisionOverlaps result;
    const auto* target = scene.get(entity);
    if (!target || !target->collider || !target->collider->enabled) {
        result.error = "entity has no enabled collider";
        return result;
    }
    if (!query_scene_valid(scene, result.error)) return result;
    initialize_jolt();
    JoltState state(assets);
    state.build(scene, true);
    if (state.over_budget()) {
        result.error = "collision query exceeds the mesh collider triangle budget";
        return result;
    }
    const auto id = state.body(entity);
    if (!id) {
        result.error = "collider has a degenerate or excessive world transform, or no usable mesh";
        return result;
    }
    const auto& api = state.system().GetBodyInterface();
    const auto shape = api.GetShape(*id);
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
    // Jolt has no triangle-mesh versus triangle-mesh test, so a mesh target skips other meshes.
    const ExcludeTriangleMeshes exclude_meshes;
    const JPH::BodyFilter any_body;
    state.system().GetNarrowPhaseQuery().CollideShape(shape.GetPtr(), JPH::Vec3::sOne(),
        api.GetCenterOfMassTransform(*id), {}, JPH::RVec3::sZero(), hits, {}, {},
        shape->GetSubType() == JPH::EShapeSubType::Mesh
            ? static_cast<const JPH::BodyFilter&>(exclude_meshes) : any_body);
    std::set<Entity> unique;
    for (const auto& hit : hits.mHits) {
        const auto other = from_user_data(api.GetUserData(hit.mBodyID2));
        const auto* record = scene.get(other);
        if (other == entity || !record || !record->collider ||
            !(target->collider->layer & record->collider->mask) ||
            !(record->collider->layer & target->collider->mask)) continue;
        unique.insert(other);
    }
    for (const auto other : unique) {
        if (result.entities.size() == 128U) { result.truncated = true; break; }
        result.entities.push_back(other);
    }
    return result;
}

} // namespace relay
