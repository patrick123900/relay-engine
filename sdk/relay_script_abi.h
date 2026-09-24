/* Relay native script ABI. The engine and compiled project scripts share only this C interface,
 * so a script library never links against engine internals and each side validates the other's
 * version and table size before use. Scripts should include relay_script.hpp instead. */
#ifndef RELAY_SCRIPT_ABI_H
#define RELAY_SCRIPT_ABI_H

#include <stddef.h>
#include <stdint.h>

#define RELAY_SCRIPT_ABI_VERSION 1u
#define RELAY_SCRIPT_ENTRY_SYMBOL "relay_script_module_v1"
#define RELAY_SCRIPT_ERROR_CAPACITY 512u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RelayVec3 { double x, y, z; } RelayVec3;

/* Entities are generation-checked handles packed as (generation << 32) | index. Zero is none. */
typedef uint64_t RelayEntity;

typedef struct RelayRayHit {
    RelayEntity entity;
    double distance;
    RelayVec3 point;
    RelayVec3 normal;
} RelayRayHit;

enum RelayLogLevel { RELAY_LOG_INFO = 1, RELAY_LOG_WARNING = 2, RELAY_LOG_ERROR = 3 };

enum RelayCallback {
    RELAY_CALLBACK_START = 0,
    RELAY_CALLBACK_UPDATE = 1,
    RELAY_CALLBACK_CONTACT_BEGIN = 2,
    RELAY_CALLBACK_CONTACT_END = 3,
    RELAY_CALLBACK_STOP = 4,
    RELAY_CALLBACK_RELOAD = 5,
    RELAY_CALLBACK_DESTROY = 6
};

enum RelayInputQuery { RELAY_INPUT_HELD = 0, RELAY_INPUT_PRESSED = 1, RELAY_INPUT_RELEASED = 2 };

enum RelayPropertyType {
    RELAY_PROPERTY_BOOLEAN = 0,
    RELAY_PROPERTY_NUMBER = 1,
    RELAY_PROPERTY_VECTOR = 2,
    RELAY_PROPERTY_TEXT = 3
};

/* One property value; only the member matching `type` is meaningful. `text` is not
 * NUL-terminated. */
typedef struct RelayPropertyValue {
    int type;
    int boolean;
    double number;
    RelayVec3 vector;
    const char* text;
    size_t text_length;
} RelayPropertyValue;

/* A declared property and its default, as written in the behaviour's code. Pointers stay valid
 * while the library is loaded. */
typedef struct RelayPropertyInfo {
    const char* name;
    RelayPropertyValue value;
} RelayPropertyInfo;

/* Functions returning int report 1 on success and 0 when the entity is gone or lacks the needed
 * component. Every call runs on the engine thread during a script callback. */
typedef struct RelayHostApi {
    uint32_t abi_version;
    uint32_t size;
    void* context;
    double (*time)(void* context);
    uint64_t (*frame)(void* context);
    void (*log)(void* context, int level, const char* text, size_t length);
    int (*alive)(void* context, RelayEntity entity);
    RelayEntity (*find)(void* context, const char* name, size_t length);
    size_t (*name)(void* context, RelayEntity entity, char* buffer, size_t capacity);
    RelayEntity (*parent)(void* context, RelayEntity entity);
    int (*get_position)(void* context, RelayEntity entity, RelayVec3* local);
    int (*set_position)(void* context, RelayEntity entity, RelayVec3 local);
    int (*get_rotation)(void* context, RelayEntity entity, RelayVec3* euler_degrees);
    int (*set_rotation)(void* context, RelayEntity entity, RelayVec3 euler_degrees);
    int (*get_scale)(void* context, RelayEntity entity, RelayVec3* scale);
    int (*set_scale)(void* context, RelayEntity entity, RelayVec3 scale);
    int (*world_position)(void* context, RelayEntity entity, RelayVec3* world);
    int (*get_velocity)(void* context, RelayEntity entity, RelayVec3* velocity);
    int (*set_velocity)(void* context, RelayEntity entity, RelayVec3 velocity);
    int (*get_angular_velocity)(void* context, RelayEntity entity, RelayVec3* radians);
    int (*set_angular_velocity)(void* context, RelayEntity entity, RelayVec3 radians);
    int (*apply_impulse)(void* context, RelayEntity entity, RelayVec3 impulse,
                         const RelayVec3* world_point);
    /* `ignore` skips one entity's collider, typically the caller's own. */
    int (*raycast)(void* context, RelayVec3 origin, RelayVec3 direction, double maximum_distance,
                   uint32_t layer_mask, RelayEntity ignore, RelayRayHit* hit);
    /* Input, latched once per game step. Actions and axes come from the project's input map;
     * controls are raw names such as key:w, mouse:left or gamepad:leftx. */
    int (*input_action)(void* context, const char* name, size_t length, int query);
    double (*input_axis)(void* context, const char* name, size_t length);
    int (*input_control)(void* context, const char* control, size_t length, int query);
    double (*input_control_value)(void* context, const char* control, size_t length);
    /* Window position in pixels, and this step's movement with the wheel in delta->z. */
    void (*input_mouse)(void* context, RelayVec3* position, RelayVec3* delta);
    /* The first direct child with this name, or 0. */
    RelayEntity (*child)(void* context, RelayEntity parent, const char* name, size_t length);
    /* Makes the entity's camera the one the game renders through, for the rest of the run. */
    int (*activate_camera)(void* context, RelayEntity entity);
    /* Scene changes during Run Game. New entities exist at once and their scripts start before
     * their first update; destruction waits until the current callbacks finish. Stop Game
     * undoes all of it. Each returns 0 on failure and logs why. */
    RelayEntity (*create_entity)(void* context, const char* name, size_t length, RelayEntity parent);
    /* Copies templates/<name>.relay-template.json under `parent` (0 for the top level). Null
     * position or rotation keeps the template's own. */
    RelayEntity (*instantiate)(void* context, const char* name, size_t length, RelayEntity parent,
                               const RelayVec3* position, const RelayVec3* rotation);
    /* Copies an entity and its descendants beside the original, keeping its name. */
    RelayEntity (*clone)(void* context, RelayEntity entity);
    int (*destroy)(void* context, RelayEntity entity);
    /* Queries that fill `out` with up to `capacity` entities, sorted, and return how many there
     * are in total. */
    size_t (*children)(void* context, RelayEntity parent, RelayEntity* out, size_t capacity);
    /* Enabled colliders overlapping the entity's own collider, filtered by both layers and masks. */
    size_t (*overlaps)(void* context, RelayEntity entity, RelayEntity* out, size_t capacity);
    /* Enabled colliders on `layer_mask` layers overlapping a world-space sphere. */
    size_t (*overlap_sphere)(void* context, RelayVec3 center, double radius, uint32_t layer_mask,
                             RelayEntity ignore, RelayEntity* out, size_t capacity);
} RelayHostApi;

/* Returned by the module entry point. `error` receives a NUL-terminated message when a call
 * returns 0; C++ exceptions never cross this boundary. */
typedef struct RelayScriptModule {
    uint32_t abi_version;
    uint32_t size;
    uint32_t (*behaviour_count)(void);
    const char* (*behaviour_name)(uint32_t index);
    void* (*create)(uint32_t behaviour, RelayEntity entity, char* error, size_t capacity);
    void (*destroy)(void* instance);
    int (*call)(void* instance, int callback, double delta_seconds, RelayEntity other, char* error,
                size_t capacity);
    uint32_t (*property_count)(uint32_t behaviour);
    int (*property_info)(uint32_t behaviour, uint32_t index, RelayPropertyInfo* info);
    /* Assigns a declared property before on_start; numbers convert to the field's numeric type. */
    int (*set_property)(void* instance, const char* name, size_t length,
                        const RelayPropertyValue* value, char* error, size_t capacity);
} RelayScriptModule;

typedef const RelayScriptModule* (*RelayScriptEntry)(const RelayHostApi* host);

#ifdef __cplusplus
}
#endif

#endif
