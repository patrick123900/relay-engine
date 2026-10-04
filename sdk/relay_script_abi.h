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

/* A convex shape for casts and overlaps. Capsules stand along their local Y axis. */
enum RelayShapeType { RELAY_SHAPE_SPHERE = 0, RELAY_SHAPE_BOX = 1, RELAY_SHAPE_CAPSULE = 2 };

typedef struct RelayShape {
    int type;
    double radius;          /* Sphere and capsule. */
    double half_height;     /* Capsule: half its cylinder, excluding the round ends; 0 is a sphere. */
    RelayVec3 half_extents; /* Box. */
    RelayVec3 rotation;     /* Euler degrees, as transforms use. */
} RelayShape;

enum RelayLogLevel { RELAY_LOG_INFO = 1, RELAY_LOG_WARNING = 2, RELAY_LOG_ERROR = 3 };

enum RelayCallback {
    RELAY_CALLBACK_START = 0,
    RELAY_CALLBACK_UPDATE = 1,
    RELAY_CALLBACK_CONTACT_BEGIN = 2,
    RELAY_CALLBACK_CONTACT_END = 3,
    RELAY_CALLBACK_STOP = 4,
    RELAY_CALLBACK_RELOAD = 5,
    RELAY_CALLBACK_DESTROY = 6,
    /* An interface event on the entity or one of its descendants; `other` is the control, and
     * ui_event() describes it during the call. */
    RELAY_CALLBACK_UI = 7
};

enum RelayUiEventType {
    RELAY_UI_CLICKED = 0,
    RELAY_UI_TOGGLED = 1,
    RELAY_UI_VALUE_CHANGED = 2,
    RELAY_UI_PRESSED = 3,
    RELAY_UI_RELEASED = 4
};

enum RelayUiState { RELAY_UI_HOVERED = 1, RELAY_UI_HELD = 2 };

enum RelayInputQuery { RELAY_INPUT_HELD = 0, RELAY_INPUT_PRESSED = 1, RELAY_INPUT_RELEASED = 2 };

enum RelayPropertyType {
    RELAY_PROPERTY_BOOLEAN = 0,
    RELAY_PROPERTY_NUMBER = 1,
    RELAY_PROPERTY_VECTOR = 2,
    RELAY_PROPERTY_TEXT = 3,
    /* A node in the scene, or a script component on one: `entity`, 0 for none. */
    RELAY_PROPERTY_ENTITY = 4,
    /* A project file or built-in asset name, in `text`. */
    RELAY_PROPERTY_ASSET = 5,
    /* An engine component id such as "camera", in `text`. */
    RELAY_PROPERTY_COMPONENT = 6
};

/* What a property's editor offers, declared in code beside the default: for an entity, nothing,
 * or only nodes running a behaviour, carrying an engine component, or of a node type (and its
 * subtypes); for an asset, the file kinds it takes as a comma separated list of the names the
 * Assets panel uses ("audio", "node_template", "image", ...). */
enum RelayPropertyFilter {
    RELAY_FILTER_NONE = 0,
    RELAY_FILTER_BEHAVIOUR = 1,
    RELAY_FILTER_COMPONENT = 2,
    RELAY_FILTER_NODE_TYPE = 3,
    RELAY_FILTER_ASSET_KINDS = 4
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
    RelayEntity entity;
} RelayPropertyValue;

/* A declared property and its default, as written in the behaviour's code. Pointers stay valid
 * while the library is loaded. */
typedef struct RelayPropertyInfo {
    const char* name;
    RelayPropertyValue value;
    int filter;            /* RelayPropertyFilter */
    const char* filter_text; /* Not NUL-terminated. */
    size_t filter_length;
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
    /* Pointer position in the game view in pixels, and this step's movement with the wheel in
     * delta->z. */
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
    /* The entity's active joint: a hinge's angle in degrees, a slider's travel in metres or a
     * distance joint's length. Returns 0 without one. */
    int (*joint_position)(void* context, RelayEntity entity, double* value);
    /* Runs a hinge or slider joint's motor at `speed` degrees or metres per second (up to its
     * authored motor force), or stops it when `on` is 0. */
    int (*set_joint_motor)(void* context, RelayEntity entity, int on, double speed);
    /* Starts, or restarts from the beginning, the entity's audio source. `volume_db` is added to
     * its authored volume and `pitch` multiplies its authored pitch, for this playing only.
     * Returns 0 without an audio source and clip, or when the clip cannot be played. */
    int (*audio_play)(void* context, RelayEntity entity, double volume_db, double pitch);
    /* Stops the entity's sound; returns whether one was playing. */
    int (*audio_stop)(void* context, RelayEntity entity);
    int (*audio_playing)(void* context, RelayEntity entity);
    /* Seconds into the entity's playing sound, or -1 when it is not playing. */
    double (*audio_position)(void* context, RelayEntity entity);
    /* Plays a project sound file once. With `position` it is placed in the world; null plays it
     * flat. Returns a sound handle, or 0 (logging why). */
    uint64_t (*audio_play_clip)(void* context, const char* clip, size_t clip_length,
                                const RelayVec3* position, double volume_db, double pitch,
                                const char* bus, size_t bus_length, double min_distance,
                                double max_distance);
    int (*audio_sound_stop)(void* context, uint64_t sound);
    int (*audio_sound_playing)(void* context, uint64_t sound);
    /* Seconds into the sound, or -1 when it is not playing. */
    double (*audio_sound_position)(void* context, uint64_t sound);
    /* Game-time mixer changes, undone when the game stops. The volume fades over
     * `fade_seconds`. */
    int (*audio_bus_volume)(void* context, const char* bus, size_t length, double volume_db,
                            double fade_seconds);
    /* The bus's current volume in dB, or -1000 without such a bus. */
    double (*audio_get_bus_volume)(void* context, const char* bus, size_t length);
    int (*audio_bus_mute)(void* context, const char* bus, size_t length, int mute);
    int (*audio_bus_effect)(void* context, const char* bus, size_t length, uint32_t index,
                            const char* parameter, size_t parameter_length, double value);
    /* Music players. `track` -1 is the next in the playlist; `sync` -1 uses the player's own
     * setting, else 0 immediately, 1 on the next beat, 2 on the next bar, 3 at the track's end. */
    int (*music_play)(void* context, RelayEntity entity, int track, int sync);
    int (*music_stop)(void* context, RelayEntity entity, double fade_seconds);
    /* The playlist index playing, or -1. */
    int (*music_track)(void* context, RelayEntity entity);
    /* Per-object values for the entity's shader material parameters (its mesh renderer's
     * .relay-material), leaving the material and other objects alone. `count` must match the
     * uniform: 1 for float, int or bool (0 or 1), 2 to 4 for vectors; colors are linear. */
    int (*set_material_parameter)(void* context, RelayEntity entity, const char* name, size_t length,
                                  const double* values, size_t count);
    /* Fills `values` with the parameter's current value (the object's own, else the material's,
     * else the shader's default) and returns how many numbers it has, or 0 without one. */
    size_t (*get_material_parameter)(void* context, RelayEntity entity, const char* name,
                                     size_t length, double* values, size_t capacity);
    /* Returns the object to the material's value; 0 when it had none of its own. */
    int (*clear_material_parameter)(void* context, RelayEntity entity, const char* name, size_t length);
    /* The game interface. Fields are named "<component>.<field>" as in the protocol's
     * scene.set_ui, such as "label.text", "control.visible", "slider.value" or "panel.color".
     * Booleans, numbers, vectors, colors and margins are numbers (booleans 0 or 1); text, file
     * paths and choices (such as "label.horizontal_align" = "center") are text. Setting checks
     * the value like the Inspector does and returns 0, logging why, when the entity lacks the
     * component or the value does not fit. Changes last until Stop Game. */
    size_t (*ui_get_numbers)(void* context, RelayEntity entity, const char* field, size_t length,
                             double* values, size_t capacity);
    int (*ui_set_numbers)(void* context, RelayEntity entity, const char* field, size_t length,
                          const double* values, size_t count);
    /* Copies up to `capacity` bytes and returns the text's full length. */
    size_t (*ui_get_text)(void* context, RelayEntity entity, const char* field, size_t length,
                          char* buffer, size_t capacity);
    int (*ui_set_text)(void* context, RelayEntity entity, const char* field, size_t length,
                       const char* text, size_t text_length);
    /* RELAY_UI_HOVERED and RELAY_UI_HELD bits for an interactive control this game step. */
    int (*ui_state)(void* context, RelayEntity entity);
    /* The event being delivered by RELAY_CALLBACK_UI; 0 outside that callback. */
    int (*ui_event)(void* context, int* type, RelayEntity* control, double* value);
    /* Shows (0) or hides and locks (1) the cursor for the rest of the game, overriding the input
     * map's lock_mouse. Controls react to the pointer only while it is unlocked. */
    int (*set_mouse_locked)(void* context, int locked);
    int (*mouse_locked)(void* context);
    /* Particle emitters. play starts the emitter's cycle (restart 1 also clears its particles);
     * stop ends emission, letting living particles finish unless `clear` is 1; emit adds `count`
     * particles now and returns how many were born. All return 0 without an emitter. */
    int (*particles_play)(void* context, RelayEntity entity, int restart);
    int (*particles_stop)(void* context, RelayEntity entity, int clear);
    int (*particles_pause)(void* context, RelayEntity entity, int paused);
    size_t (*particles_emit)(void* context, RelayEntity entity, size_t count);
    /* Living particles, and whether the emitter is emitting. */
    size_t (*particles_count)(void* context, RelayEntity entity);
    int (*particles_playing)(void* context, RelayEntity entity);
    /* Emitter fields by the names scene.set_particle_emitter uses, such as "rate", "color" or
     * "gravity". Booleans are 0 or 1, ranges two numbers (min, max), vectors three and colors four
     * (sRGB red, green, blue, alpha); choices and paths are text. Setting checks the value like the
     * Inspector does, returning 0 and logging why when it does not fit. Changes last until Stop
     * Game. Curves, gradients and bursts are edited in the scene, not from scripts. */
    size_t (*particles_get_numbers)(void* context, RelayEntity entity, const char* field, size_t length,
                                    double* values, size_t capacity);
    int (*particles_set_numbers)(void* context, RelayEntity entity, const char* field, size_t length,
                                 const double* values, size_t count);
    int (*particles_set_text)(void* context, RelayEntity entity, const char* field, size_t length,
                              const char* text, size_t text_length);
    /* Moves the entity under `parent` (0 for the top level). With `keep_world` 1 it keeps its
     * place, turn and size in the world; with 0 it keeps its local transform. Refuses cycles. */
    int (*set_parent)(void* context, RelayEntity entity, RelayEntity parent, int keep_world);
    /* Engine components by id, as component.add names them: "collider", "physics_body", "light",
     * "ui_label" and so on. Adding uses the editor's defaults; scripts use add_script and
     * remove_script instead. Changes last until Stop Game. */
    int (*has_component)(void* context, RelayEntity entity, const char* id, size_t length);
    int (*add_component)(void* context, RelayEntity entity, const char* id, size_t length);
    int (*remove_component)(void* context, RelayEntity entity, const char* id, size_t length);
    /* Attaches a behaviour; its instance starts before its first update. */
    int (*add_script)(void* context, RelayEntity entity, const char* behaviour, size_t length);
    /* Removes the entity's first script component running `behaviour` once the current
     * callbacks finish, after its on_destroy. */
    int (*remove_script)(void* context, RelayEntity entity, const char* behaviour, size_t length);
    /* Component fields named "<component>.<field>", such as "collider.radius", "light.color" or
     * "physics_body.type". Booleans, numbers and vectors are numbers; text and choices are text.
     * Setting checks the value like the scene does and returns 0, logging why, when it does not
     * fit. */
    size_t (*component_get_numbers)(void* context, RelayEntity entity, const char* field, size_t length,
                                    double* values, size_t capacity);
    int (*component_set_numbers)(void* context, RelayEntity entity, const char* field, size_t length,
                                 const double* values, size_t count);
    /* Copies up to `capacity` bytes and returns the text's full length. */
    size_t (*component_get_text)(void* context, RelayEntity entity, const char* field, size_t length,
                                 char* buffer, size_t capacity);
    int (*component_set_text)(void* context, RelayEntity entity, const char* field, size_t length,
                              const char* text, size_t text_length);
    /* Connects the entity's joint to `other`'s body, or to the world with 0. */
    int (*joint_connect)(void* context, RelayEntity entity, RelayEntity other);
    /* Sweeps `shape` from `origin` along `direction` against the running physics world. `hit`
     * gets the distance the shape's centre travelled, the contact point and the surface normal. */
    int (*shape_cast)(void* context, const RelayShape* shape, RelayVec3 origin, RelayVec3 direction,
                      double maximum_distance, uint32_t layer_mask, RelayEntity ignore, RelayRayHit* hit);
    /* Enabled colliders on `layer_mask` layers overlapping `shape` centred at `center`. */
    size_t (*overlap_shape)(void* context, const RelayShape* shape, RelayVec3 center, uint32_t layer_mask,
                            RelayEntity ignore, RelayEntity* out, size_t capacity);
    /* Script instances. The pointer is the object `RelayScriptModule::create` returned for the
     * entity's first running script component of that behaviour, or null when it has none (or the
     * instance failed or is going). It stays valid until the next scene change that removes the
     * script, so scripts look it up when they use it. */
    void* (*script_instance)(void* context, RelayEntity entity, const char* behaviour, size_t length);
    /* Entities with a running script component of that behaviour, sorted; returns the total. */
    size_t (*script_entities)(void* context, const char* behaviour, size_t length, RelayEntity* out,
                              size_t capacity);
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
