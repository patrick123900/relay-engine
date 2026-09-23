import { z } from "zod";
export function registerGeneratedTools(server, invoke, overrides = {}) {
    server.registerTool("runtime_status", {
        title: "Inspect Relay runtime",
        description: "Read the current editor or game mode, pause, frame, simulation time and resolution state.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["runtime_status"];
        if (override)
            return override({});
        return invoke("runtime.status", {});
    });
    server.registerTool("runtime_play", {
        title: "Run game",
        description: "Start a temporary game session from the authored scene. Stop restores the authored scene and discards runtime changes.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["runtime_play"];
        if (override)
            return override({});
        return invoke("runtime.play", {});
    });
    server.registerTool("runtime_stop", {
        title: "Stop game",
        description: "Stop the current game session and restore the authored scene without changing undo history.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["runtime_stop"];
        if (override)
            return override({});
        return invoke("runtime.stop", {});
    });
    server.registerTool("runtime_pause", {
        title: "Pause Relay runtime",
        description: "Pause a running game so its scene can be inspected deterministically.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false, idempotentHint: true },
    }, async () => {
        const override = overrides["runtime_pause"];
        if (override)
            return override({});
        return invoke("runtime.pause", {});
    });
    server.registerTool("runtime_resume", {
        title: "Resume Relay runtime",
        description: "Resume a paused game after inspection or a controlled frame step.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false, idempotentHint: true },
    }, async () => {
        const override = overrides["runtime_resume"];
        if (override)
            return override({});
        return invoke("runtime.resume", {});
    });
    server.registerTool("runtime_step", {
        title: "Step Relay frames",
        description: "Advance an exact number of deterministic game frames, including while paused.",
        inputSchema: z.object({
            "frames": z.number().int().min(1).max(10000).default(1).describe("Number of frames to advance")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["runtime_step"];
        if (override)
            return override(input);
        return invoke("runtime.step", { "frames": input["frames"] });
    });
    server.registerTool("runtime_shutdown", {
        title: "Shut down Relay runtime",
        description: "Request an orderly shutdown of the runtime owned by this MCP bridge.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false, idempotentHint: true },
    }, async () => {
        const override = overrides["runtime_shutdown"];
        if (override)
            return override({});
        return invoke("runtime.quit", {});
    });
    server.registerTool("editor_camera_status", {
        title: "Inspect editor camera",
        description: "Read the live editor inspection viewpoint used by Vulkan captures. Unavailable without an editor. Does not modify scene cameras.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["editor_camera_status"];
        if (override)
            return override({});
        return invoke("editor.camera.status", {});
    });
    server.registerTool("editor_camera_set", {
        title: "Position editor inspection camera",
        description: "Set the live inspector camera target, orbit angles in radians and distance for visual confirmation using render_capture source vulkan. View-only: no scene or undo changes.",
        inputSchema: z.object({
            "target_x": z.number().finite().min(-1000000).max(1000000).optional(),
            "target_y": z.number().finite().min(-1000000).max(1000000).optional(),
            "target_z": z.number().finite().min(-1000000).max(1000000).optional(),
            "yaw": z.number().finite().min(-1000).max(1000).optional(),
            "pitch": z.number().finite().min(-1.55).max(1.55).optional(),
            "distance": z.number().finite().min(0.25).max(5000).optional(),
            "mode": z.enum(["inspector", "scene"]).default("inspector")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["editor_camera_set"];
        if (override)
            return override(input);
        return invoke("editor.camera.set", { "target_x": input["target_x"], "target_y": input["target_y"], "target_z": input["target_z"], "yaw": input["yaw"], "pitch": input["pitch"], "distance": input["distance"], "mode": input["mode"] });
    });
    server.registerTool("editor_camera_frame", {
        title: "Frame entity in inspection camera",
        description: "Frame an entity and its descendant bounds in the live inspection view before Vulkan capture. Does not change selection, scene cameras or undo history.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$"))
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["editor_camera_frame"];
        if (override)
            return override(input);
        return invoke("editor.camera.frame", { "entity": input["entity"] });
    });
    server.registerTool("render_capture", {
        title: "Capture Relay frame",
        description: "Save the current rendered frame in Relay's captures directory for visual inspection.",
        inputSchema: z.object({
            "filename": z.string().regex(new RegExp("^[A-Za-z0-9][A-Za-z0-9._-]*\\.(bmp|png)$")).default("agent-capture.png").describe("Safe PNG or BMP filename without directory components"),
            "source": z.enum(["vulkan", "deterministic"]).default("vulkan").describe("Vulkan captures a real GPU frame; deterministic uses the headless CPU oracle")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["render_capture"];
        if (override)
            return override(input);
        return invoke("render.capture", { "path": input["filename"], "source": input["source"] });
    });
    server.registerTool("render_capture_async", {
        title: "Queue Relay frame capture",
        description: "Capture real Vulkan or deterministic CPU frames through bounded background image workers.",
        inputSchema: z.object({
            "filename": z.string().regex(new RegExp("^[A-Za-z0-9][A-Za-z0-9._-]*\\.(bmp|png)$")).default("agent-async.png").describe("Safe PNG or BMP filename without directory components"),
            "source": z.enum(["vulkan", "deterministic"]).optional().describe("Capture provenance; omitted uses the active renderer. Vulkan requires a live editor.")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["render_capture_async"];
        if (override)
            return override(input);
        return invoke("render.capture_async", { "path": input["filename"], "source": input["source"] });
    });
    server.registerTool("render_capture_cancel", {
        title: "Cancel pending capture",
        description: "Cancel a queued image or GPU readback job; writing and completed jobs cannot be cancelled. GPU slots remain alive until their fence completes.",
        inputSchema: z.object({
            "job": z.number().int().min(1)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["render_capture_cancel"];
        if (override)
            return override(input);
        return invoke("render.capture_cancel", { "job": input["job"] });
    });
    server.registerTool("render_capture_status", {
        title: "Inspect capture job",
        description: "Check whether an asynchronous capture is queued, writing, complete or failed.",
        inputSchema: z.object({
            "job": z.number().int().min(1)
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["render_capture_status"];
        if (override)
            return override(input);
        return invoke("render.capture_status", { "job": input["job"] });
    });
    server.registerTool("render_capabilities", {
        title: "Inspect graphics capabilities",
        description: "Create a temporary Vulkan device and report adapter and modern rendering support.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["render_capabilities"];
        if (override)
            return override({});
        return invoke("render.capabilities", {});
    });
    server.registerTool("render_graph", {
        title: "Inspect render graph",
        description: "Read compiled render passes, resources, dependencies and access transitions.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["render_graph"];
        if (override)
            return override({});
        return invoke("render.graph", {});
    });
    server.registerTool("render_shader_interfaces", {
        title: "Inspect shader interfaces",
        description: "Read SPIR-V-reflected stages, locations, descriptor bindings and push-constant sizes from the live Vulkan pipeline.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["render_shader_interfaces"];
        if (override)
            return override({});
        return invoke("render.shader_interfaces", {});
    });
    server.registerTool("render_upload_status", {
        title: "Inspect upload memory",
        description: "Read Vulkan asset upload budgets, staging peaks, estimated resident bytes and transfer-path state.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["render_upload_status"];
        if (override)
            return override({});
        return invoke("render.upload_status", {});
    });
    server.registerTool("render_assets", {
        title: "Inspect render assets",
        description: "List built-in and imported meshes, materials and textures currently available to scene renderer components.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["render_assets"];
        if (override)
            return override({});
        return invoke("render.assets", {});
    });
    server.registerTool("asset_import_formats", {
        title: "Inspect model import formats",
        description: "Report model formats and feature coverage available in this Relay build, including Godot-compatible interchange paths.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["asset_import_formats"];
        if (override)
            return override({});
        return invoke("assets.formats", {});
    });
    server.registerTool("asset_import_model", {
        title: "Import project model",
        description: "Import a model from the project-local assets directory using a content-addressed identity and optionally instantiate its node hierarchy.",
        inputSchema: z.object({
            "filename": z.string().min(5).max(128).regex(new RegExp("^[A-Za-z0-9][A-Za-z0-9._ /-]*\\.(gltf|glb|fbx|obj|dae|blend)$")).describe("Project-relative model path; traversal and symlinks are rejected"),
            "instantiate": z.boolean().default(true).describe("Create the imported model hierarchy in the active scene"),
            "preset": z.enum(["scene", "static_mesh"]).default("scene").describe("Scene preserves conversion-time animation, camera and light data; static_mesh strips those channels")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["asset_import_model"];
        if (override)
            return override(input);
        return invoke("assets.import_model", { "filename": input["filename"], "instantiate": input["instantiate"], "preset": input["preset"] });
    });
    server.registerTool("logs_read", {
        title: "Read Relay logs",
        description: "Read structured engine log entries newer than a sequence number.",
        inputSchema: z.object({
            "after": z.number().int().min(0).default(0).describe("Last sequence number already seen")
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["logs_read"];
        if (override)
            return override(input);
        return invoke("logs.read", { "after": input["after"] });
    });
    server.registerTool("performance_read", {
        title: "Read Relay performance",
        description: "Read bounded per-frame CPU/GPU timing, draw/resource counts, entities and process memory.",
        inputSchema: z.object({
            "afterFrame": z.number().int().min(0).default(0),
            "limit": z.number().int().min(1).max(240).default(30)
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["performance_read"];
        if (override)
            return override(input);
        return invoke("performance.read", { "after_frame": input["afterFrame"], "limit": input["limit"] });
    });
    server.registerTool("input_recent", {
        title: "Read recent Relay input",
        description: "Read the bounded normalized keyboard, mouse and gamepad input event history.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["input_recent"];
        if (override)
            return override({});
        return invoke("input.recent", {});
    });
    server.registerTool("video_start", {
        title: "Start Relay video",
        description: "Record real Vulkan or deterministic CPU frames to WebM with explicit frame drops.",
        inputSchema: z.object({
            "filename": z.string().regex(new RegExp("^[A-Za-z0-9][A-Za-z0-9._-]*\\.webm$")).default("agent-recording.webm").describe("Safe WebM filename without directory components"),
            "fps": z.number().int().min(1).max(60).default(30),
            "maximumFrames": z.number().int().min(1).max(3600).default(300),
            "source": z.enum(["vulkan", "deterministic"]).optional().describe("Capture provenance; omitted uses the active renderer. Vulkan requires a live editor.")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["video_start"];
        if (override)
            return override(input);
        return invoke("video.start", { "filename": input["filename"], "fps": input["fps"], "maximum_frames": input["maximumFrames"], "source": input["source"] });
    });
    server.registerTool("video_capabilities", {
        title: "Inspect video capabilities",
        description: "Report whether this Relay build found the FFmpeg WebM encoder.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["video_capabilities"];
        if (override)
            return override({});
        return invoke("video.capabilities", {});
    });
    server.registerTool("video_stop", {
        title: "Stop Relay video",
        description: "Drain pending readbacks and start background WebM finalization; poll video.status for completion and errors.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["video_stop"];
        if (override)
            return override({});
        return invoke("video.stop", {});
    });
    server.registerTool("video_status", {
        title: "Inspect Relay video",
        description: "Read recording state, submitted frames and explicitly reported frame drops.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["video_status"];
        if (override)
            return override({});
        return invoke("video.status", {});
    });
    server.registerTool("scene_list", {
        title: "List scene entities",
        description: "List every live entity with its name, parent and local transform.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["scene_list"];
        if (override)
            return override({});
        return invoke("scene.list", {});
    });
    server.registerTool("scene_inspect", {
        title: "Inspect scene entity",
        description: "Inspect one entity using its stable generation-checked handle.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")).describe("Generation-checked Relay entity handle such as 0:1")
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_inspect"];
        if (override)
            return override(input);
        return invoke("scene.inspect", { "entity": input["entity"] });
    });
    server.registerTool("scene_create", {
        title: "Create scene entity",
        description: "Create a named entity, optionally parented to another live entity.",
        inputSchema: z.object({
            "name": z.string().min(1).max(128).default("Entity"),
            "parent": z.string().regex(new RegExp("^\\d+:\\d+$")).optional().describe("Optional parent entity handle")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_create"];
        if (override)
            return override(input);
        return invoke("scene.create", { "name": input["name"], "parent": input["parent"] });
    });
    server.registerTool("scene_destroy", {
        title: "Destroy scene entity",
        description: "Destroy an entity and its descendants as one undoable transaction.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$"))
        }),
        annotations: { readOnlyHint: false, destructiveHint: true, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_destroy"];
        if (override)
            return override(input);
        return invoke("scene.destroy", { "entity": input["entity"] });
    });
    server.registerTool("scene_duplicate", {
        title: "Duplicate scene entity",
        description: "Copy an entity and its descendants beside the original as one undoable transaction. Components are preserved, a copied camera is never the active one, and a copy of a whole imported model drives its own animation.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$"))
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_duplicate"];
        if (override)
            return override(input);
        return invoke("scene.duplicate", { "entity": input["entity"] });
    });
    server.registerTool("scene_clear", {
        title: "Clear the scene",
        description: "Destroy every entity as one undoable transaction, leaving an empty scene to start new work in.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: true, openWorldHint: false },
    }, async () => {
        const override = overrides["scene_clear"];
        if (override)
            return override({});
        return invoke("scene.clear", {});
    });
    server.registerTool("scene_set_transform", {
        title: "Set entity transform",
        description: "Update selected local position, Euler rotation or scale fields in one transaction.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "px": z.number().finite().optional(),
            "py": z.number().finite().optional(),
            "pz": z.number().finite().optional(),
            "rx": z.number().finite().optional(),
            "ry": z.number().finite().optional(),
            "rz": z.number().finite().optional(),
            "sx": z.number().finite().optional(),
            "sy": z.number().finite().optional(),
            "sz": z.number().finite().optional(),
            "gesture": z.number().int().min(0).max(1000000000).default(0).describe("Nonzero token identifying one continuous drag. Updates sharing a token fold into a single undo step; use a fresh token per gesture")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_transform"];
        if (override)
            return override(input);
        return invoke("scene.set_transform", { "entity": input["entity"], "px": input["px"], "py": input["py"], "pz": input["pz"], "rx": input["rx"], "ry": input["ry"], "rz": input["rz"], "sx": input["sx"], "sy": input["sy"], "sz": input["sz"], "gesture": input["gesture"] });
    });
    server.registerTool("scene_set_camera", {
        title: "Configure entity camera",
        description: "Add, update or remove a perspective or orthographic camera; activating one deactivates the previous camera.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "enabled": z.boolean().default(true),
            "active": z.boolean().default(true),
            "fieldOfViewY": z.number().finite().min(1.01).max(178.99).default(60),
            "nearPlane": z.number().finite().min(0.0001).max(1000).default(0.1),
            "farPlane": z.number().finite().min(0.001).max(1000000).default(1000),
            "orthographicHeight": z.number().finite().min(0).max(1000000).optional().describe("Full viewport height; zero selects perspective"),
            "exposureEv": z.number().finite().min(-16).max(16).optional().describe("Exposure compensation in stops; zero is the reference exposure")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_camera"];
        if (override)
            return override(input);
        return invoke("scene.set_camera", { "entity": input["entity"], "enabled": input["enabled"], "active": input["active"], "field_of_view_y_degrees": input["fieldOfViewY"], "near_plane": input["nearPlane"], "far_plane": input["farPlane"], "orthographic_height": input["orthographicHeight"], "exposure_ev": input["exposureEv"] });
    });
    server.registerTool("scene_set_renderer", {
        title: "Configure entity renderer",
        description: "Attach a registered built-in or imported mesh and material to an entity, or remove its renderer component.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "enabled": z.boolean().default(true),
            "mesh": z.string().max(128).regex(new RegExp("^[A-Za-z0-9._:-]+$")).default("builtin.triangle"),
            "material": z.string().max(128).regex(new RegExp("^[A-Za-z0-9._:-]+$")).default("builtin.orange")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_renderer"];
        if (override)
            return override(input);
        return invoke("scene.set_renderer", { "entity": input["entity"], "enabled": input["enabled"], "mesh": input["mesh"], "material": input["material"] });
    });
    server.registerTool("scene_set_parent", {
        title: "Set entity parent",
        description: "Reparent an entity safely; use null to move it to the scene root.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "parent": z.string().regex(new RegExp("^\\d+:\\d+$")).nullable()
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_parent"];
        if (override)
            return override(input);
        return invoke("scene.set_parent", { "entity": input["entity"], "parent": input["parent"] });
    });
    server.registerTool("scene_undo", {
        title: "Undo scene change",
        description: "Undo the most recent scene transaction and restore exact entity generations.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["scene_undo"];
        if (override)
            return override({});
        return invoke("scene.undo", {});
    });
    server.registerTool("scene_redo", {
        title: "Redo scene change",
        description: "Reapply the most recently undone scene transaction.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["scene_redo"];
        if (override)
            return override({});
        return invoke("scene.redo", {});
    });
    server.registerTool("scene_set_animation", {
        title: "Control imported animation",
        description: "Configure clip, playback, looping, speed and seek time on an imported model root.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "clip": z.number().int().min(0).max(255).optional(),
            "playing": z.boolean().optional(),
            "loop": z.boolean().optional(),
            "speed": z.number().finite().min(-100).max(100).optional(),
            "timeSeconds": z.number().finite().min(0).max(1000000).optional(),
            "gesture": z.number().int().min(0).max(4294967295).optional().describe("Shared token for updates in one scrub gesture; zero creates a separate undo entry")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_animation"];
        if (override)
            return override(input);
        return invoke("scene.set_animation", { "entity": input["entity"], "clip": input["clip"], "playing": input["playing"], "loop": input["loop"], "speed": input["speed"], "time_seconds": input["timeSeconds"], "gesture": input["gesture"] });
    });
    server.registerTool("scene_set_morph", {
        title: "Set imported morph weight",
        description: "Override a mesh morph weight, or reset all overrides to imported defaults and animation.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "target": z.number().int().min(0).max(63).default(0),
            "weight": z.number().finite().min(-100).max(100).default(0),
            "reset": z.boolean().default(false)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_morph"];
        if (override)
            return override(input);
        return invoke("scene.set_morph", { "entity": input["entity"], "target": input["target"], "weight": input["weight"], "reset": input["reset"] });
    });
    server.registerTool("scene_set_collider", {
        title: "Configure box collider",
        description: "Add, edit or remove an authored box collider. The box follows the entity hierarchy and scene-owned transform animation. Undoable and saved with the scene.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "attached": z.boolean().optional(),
            "enabled": z.boolean().optional(),
            "centerX": z.number().finite().min(-1000000).max(1000000).optional(),
            "centerY": z.number().finite().min(-1000000).max(1000000).optional(),
            "centerZ": z.number().finite().min(-1000000).max(1000000).optional(),
            "halfX": z.number().finite().min(1e-06).max(1000000).optional(),
            "halfY": z.number().finite().min(1e-06).max(1000000).optional(),
            "halfZ": z.number().finite().min(1e-06).max(1000000).optional(),
            "layer": z.number().int().min(1).max(4294967295).optional(),
            "mask": z.number().int().min(0).max(4294967295).optional()
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_collider"];
        if (override)
            return override(input);
        return invoke("scene.set_collider", { "entity": input["entity"], "attached": input["attached"], "enabled": input["enabled"], "center_x": input["centerX"], "center_y": input["centerY"], "center_z": input["centerZ"], "half_x": input["halfX"], "half_y": input["halfY"], "half_z": input["halfZ"], "layer": input["layer"], "mask": input["mask"] });
    });
    server.registerTool("physics_raycast", {
        title: "Raycast box colliders",
        description: "Find the nearest enabled authored box collider hit by a world-space ray. Returns hit point and surface normal; does not move objects.",
        inputSchema: z.object({
            "originX": z.number().finite().min(-1000000).max(1000000),
            "originY": z.number().finite().min(-1000000).max(1000000),
            "originZ": z.number().finite().min(-1000000).max(1000000),
            "directionX": z.number().finite().min(-1000000).max(1000000),
            "directionY": z.number().finite().min(-1000000).max(1000000),
            "directionZ": z.number().finite().min(-1000000).max(1000000),
            "maximumDistance": z.number().finite().min(0).max(1000000).optional(),
            "layerMask": z.number().int().min(0).max(4294967295).optional()
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["physics_raycast"];
        if (override)
            return override(input);
        return invoke("physics.raycast", { "origin_x": input["originX"], "origin_y": input["originY"], "origin_z": input["originZ"], "direction_x": input["directionX"], "direction_y": input["directionY"], "direction_z": input["directionZ"], "maximum_distance": input["maximumDistance"], "layer_mask": input["layerMask"] });
    });
    server.registerTool("physics_overlaps", {
        title: "Find overlapping colliders",
        description: "List up to 128 enabled box colliders overlapping an entity collider. Both colliders must pass their layer masks; reports truncation.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$"))
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["physics_overlaps"];
        if (override)
            return override(input);
        return invoke("physics.overlaps", { "entity": input["entity"] });
    });
    server.registerTool("physics_body_status", {
        title: "Inspect physics body",
        description: "Read the current linear and angular velocities of a dynamic body during Run Game.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$"))
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["physics_body_status"];
        if (override)
            return override(input);
        return invoke("physics.body_status", { "entity": input["entity"] });
    });
    server.registerTool("physics_apply_impulse", {
        title: "Apply rigid body impulse",
        description: "Apply a world-space impulse to a dynamic body during Run Game. Optional world-space point produces torque and angular motion.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "impulseX": z.number().finite().min(-1000000).max(1000000),
            "impulseY": z.number().finite().min(-1000000).max(1000000),
            "impulseZ": z.number().finite().min(-1000000).max(1000000),
            "pointX": z.number().finite().min(-1000000).max(1000000).optional(),
            "pointY": z.number().finite().min(-1000000).max(1000000).optional(),
            "pointZ": z.number().finite().min(-1000000).max(1000000).optional()
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["physics_apply_impulse"];
        if (override)
            return override(input);
        return invoke("physics.apply_impulse", { "entity": input["entity"], "impulse_x": input["impulseX"], "impulse_y": input["impulseY"], "impulse_z": input["impulseZ"], "point_x": input["pointX"], "point_y": input["pointY"], "point_z": input["pointZ"] });
    });
    server.registerTool("scene_set_physics_body", {
        title: "Configure physics body",
        description: "Add, edit or remove an undoable static or dynamic physics body. Dynamic bodies use Jolt rigid-body collision, gravity and angular dynamics during Run Game.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "attached": z.boolean().optional(),
            "type": z.enum(["static", "dynamic"]).optional(),
            "mass": z.number().finite().min(1e-06).max(1000000).optional(),
            "gravityScale": z.number().finite().min(0).max(100).optional(),
            "restitution": z.number().finite().min(0).max(1).optional(),
            "friction": z.number().finite().min(0).max(10).optional(),
            "linearDamping": z.number().finite().min(0).max(100).optional(),
            "angularDamping": z.number().finite().min(0).max(100).optional()
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_physics_body"];
        if (override)
            return override(input);
        return invoke("scene.set_physics_body", { "entity": input["entity"], "attached": input["attached"], "type": input["type"], "mass": input["mass"], "gravity_scale": input["gravityScale"], "restitution": input["restitution"], "friction": input["friction"], "linear_damping": input["linearDamping"], "angular_damping": input["angularDamping"] });
    });
    server.registerTool("scene_set_light", {
        title: "Configure scene light",
        description: "Add, update or remove a directional, point or spot light.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "enabled": z.boolean().default(true),
            "type": z.enum(["directional", "point", "spot"]).optional(),
            "red": z.number().finite().min(0).max(100000).optional(),
            "green": z.number().finite().min(0).max(100000).optional(),
            "blue": z.number().finite().min(0).max(100000).optional(),
            "intensity": z.number().finite().min(0).max(100000).optional(),
            "constant": z.number().finite().min(0).max(100000).optional(),
            "linear": z.number().finite().min(0).max(100000).optional(),
            "quadratic": z.number().finite().min(0).max(100000).optional(),
            "innerCone": z.number().finite().min(0).max(1.5707963267948966).optional(),
            "outerCone": z.number().finite().min(0.0001).max(1.5707963267948966).optional(),
            "range": z.number().finite().min(0).max(1000000).optional().describe("Light range; zero means infinite")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_light"];
        if (override)
            return override(input);
        return invoke("scene.set_light", { "entity": input["entity"], "enabled": input["enabled"], "type": input["type"], "red": input["red"], "green": input["green"], "blue": input["blue"], "intensity": input["intensity"], "constant": input["constant"], "linear": input["linear"], "quadratic": input["quadratic"], "inner_cone": input["innerCone"], "outer_cone": input["outerCone"], "range": input["range"] });
    });
    server.registerTool("scene_rename", {
        title: "Rename scene entity",
        description: "Change an entity's display name as one undoable transaction.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "name": z.string().min(1).max(128)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_rename"];
        if (override)
            return override(input);
        return invoke("scene.rename", { "entity": input["entity"], "name": input["name"] });
    });
    server.registerTool("scene_history", {
        title: "Inspect undo history",
        description: "Read the labels currently on the undo and redo stacks, newest first.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["scene_history"];
        if (override)
            return override({});
        return invoke("scene.history", {});
    });
    server.registerTool("asset_available_models", {
        title: "List importable models",
        description: "List model files present in the project assets directory that this build can import. Top-level files only; the import sandbox is unchanged.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["asset_available_models"];
        if (override)
            return override({});
        return invoke("assets.available", {});
    });
    server.registerTool("scene_pick", {
        title: "Pick scene entity",
        description: "Find the nearest drawable entity a world-space ray enters. Bounds-level precision, not per-triangle. Stateless: the caller supplies the ray, so the engine stores no viewpoint.",
        inputSchema: z.object({
            "originX": z.number().finite().min(-1000000).max(1000000),
            "originY": z.number().finite().min(-1000000).max(1000000),
            "originZ": z.number().finite().min(-1000000).max(1000000),
            "directionX": z.number().finite().min(-1000000).max(1000000),
            "directionY": z.number().finite().min(-1000000).max(1000000),
            "directionZ": z.number().finite().min(-1000000).max(1000000)
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_pick"];
        if (override)
            return override(input);
        return invoke("scene.pick", { "origin_x": input["originX"], "origin_y": input["originY"], "origin_z": input["originZ"], "direction_x": input["directionX"], "direction_y": input["directionY"], "direction_z": input["directionZ"] });
    });
    server.registerTool("scene_bounds", {
        title: "Inspect entity bounds",
        description: "Read the world-space axis-aligned bounds and origin of an entity and its descendants, for framing a selection or locating an object.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$"))
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_bounds"];
        if (override)
            return override(input);
        return invoke("scene.bounds", { "entity": input["entity"] });
    });
    server.registerTool("scene_snapshot", {
        title: "Snapshot Relay scene",
        description: "Return deterministic scene JSON plus reflected component field metadata.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["scene_snapshot"];
        if (override)
            return override({});
        return invoke("scene.snapshot", {});
    });
    server.registerTool("scene_save", {
        title: "Save Relay scene",
        description: "Atomically save the current scene in Relay's project-local scenes directory.",
        inputSchema: z.object({
            "filename": z.string().min(12).max(128).regex(new RegExp("^[A-Za-z0-9][A-Za-z0-9._-]*\\.relay\\.json$")).default("main.relay.json").describe("Safe scene filename without directory components")
        }),
        annotations: { readOnlyHint: false, destructiveHint: true, openWorldHint: false, idempotentHint: true },
    }, async (input) => {
        const override = overrides["scene_save"];
        if (override)
            return override(input);
        return invoke("scene.save", { "filename": input["filename"] });
    });
    server.registerTool("scene_load", {
        title: "Load Relay scene",
        description: "Validate, migrate and load a project scene as one undoable transaction.",
        inputSchema: z.object({
            "filename": z.string().min(12).max(128).regex(new RegExp("^[A-Za-z0-9][A-Za-z0-9._-]*\\.relay\\.json$"))
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_load"];
        if (override)
            return override(input);
        return invoke("scene.load", { "filename": input["filename"] });
    });
    server.registerTool("trace_start", {
        title: "Start Relay trace",
        description: "Record frame-stamped agent commands and normalized human input events.",
        inputSchema: z.object({
            "filename": z.string().regex(new RegExp("^[A-Za-z0-9][A-Za-z0-9._-]*\\.relay-trace\\.jsonl$")).default("agent.relay-trace.jsonl").describe("Safe Relay trace filename without directory components")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["trace_start"];
        if (override)
            return override(input);
        return invoke("trace.start", { "filename": input["filename"] });
    });
    server.registerTool("trace_stop", {
        title: "Stop Relay trace",
        description: "Atomically persist the active deterministic trace in Relay's traces directory.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["trace_stop"];
        if (override)
            return override({});
        return invoke("trace.stop", {});
    });
    server.registerTool("trace_status", {
        title: "Inspect Relay trace",
        description: "Read trace recording state, path and current event count.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["trace_status"];
        if (override)
            return override({});
        return invoke("trace.status", {});
    });
    server.registerTool("trace_replay", {
        title: "Replay Relay trace",
        description: "Validate and replay a trace against the fixed-step runtime at its recorded frame offsets.",
        inputSchema: z.object({
            "filename": z.string().regex(new RegExp("^[A-Za-z0-9][A-Za-z0-9._-]*\\.relay-trace\\.jsonl$"))
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["trace_replay"];
        if (override)
            return override(input);
        return invoke("trace.replay", { "filename": input["filename"] });
    });
    server.registerTool("scene_copy", {
        title: "Copy",
        description: "Copy selected subtrees to the session clipboard without changing the scene.",
        inputSchema: z.object({
            "entities": z.array(z.string().regex(new RegExp("^\\d+:\\d+$"))).min(1).max(4096)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_copy"];
        if (override)
            return override(input);
        return invoke("scene.copy", { "entities": input["entities"] });
    });
    server.registerTool("scene_cut", {
        title: "Cut",
        description: "Copy and remove selected subtrees as one undoable edit.",
        inputSchema: z.object({
            "entities": z.array(z.string().regex(new RegExp("^\\d+:\\d+$"))).min(1).max(4096)
        }),
        annotations: { readOnlyHint: false, destructiveHint: true, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_cut"];
        if (override)
            return override(input);
        return invoke("scene.cut", { "entities": input["entities"] });
    });
    server.registerTool("scene_duplicate_many", {
        title: "Duplicate Many",
        description: "Duplicate selected subtrees beside their originals as one undoable edit.",
        inputSchema: z.object({
            "entities": z.array(z.string().regex(new RegExp("^\\d+:\\d+$"))).min(1).max(4096)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_duplicate_many"];
        if (override)
            return override(input);
        return invoke("scene.duplicate_many", { "entities": input["entities"] });
    });
    server.registerTool("scene_destroy_many", {
        title: "Destroy Many",
        description: "Remove selected subtrees as one undoable edit.",
        inputSchema: z.object({
            "entities": z.array(z.string().regex(new RegExp("^\\d+:\\d+$"))).min(1).max(4096)
        }),
        annotations: { readOnlyHint: false, destructiveHint: true, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_destroy_many"];
        if (override)
            return override(input);
        return invoke("scene.destroy_many", { "entities": input["entities"] });
    });
    server.registerTool("scene_transform_many", {
        title: "Transform Many",
        description: "Apply a column-major world-space affine delta matrix to selected roots as one undoable gesture.",
        inputSchema: z.object({
            "entities": z.array(z.string().regex(new RegExp("^\\d+:\\d+$"))).min(1).max(4096),
            "delta": z.array(z.number().finite()).min(16).max(16),
            "gesture": z.number().int().min(0).max(4294967295).optional()
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_transform_many"];
        if (override)
            return override(input);
        return invoke("scene.transform_many", { "entities": input["entities"], "delta": input["delta"], "gesture": input["gesture"] });
    });
    server.registerTool("scene_paste", {
        title: "Paste entities",
        description: "Paste the session clipboard as one undoable edit; cameras stay inactive and internal model bindings are remapped.",
        inputSchema: z.object({
            "parent": z.string().regex(new RegExp("^\\d+:\\d+$")).nullable().optional()
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_paste"];
        if (override)
            return override(input);
        return invoke("scene.paste", { "parent": input["parent"] });
    });
    server.registerTool("scene_clipboard", {
        title: "Inspect clipboard",
        description: "Read session clipboard root and entity counts.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["scene_clipboard"];
        if (override)
            return override({});
        return invoke("scene.clipboard", {});
    });
    server.registerTool("project_create", {
        title: "Create project",
        description: "Create a folder project (.relayproject) and an empty scene; refuses to overwrite an existing project.",
        inputSchema: z.object({
            "filename": z.string().min(1).max(128),
            "name": z.string().min(1).max(128)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["project_create"];
        if (override)
            return override(input);
        return invoke("project.create", { "filename": input["filename"], "name": input["name"] });
    });
    server.registerTool("project_open", {
        title: "Open project",
        description: "Open a folder project (.relayproject) and load its startup scene atomically, or clear the scene for an empty project.",
        inputSchema: z.object({
            "filename": z.string().min(1).max(128)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["project_open"];
        if (override)
            return override(input);
        return invoke("project.open", { "filename": input["filename"] });
    });
    server.registerTool("project_status", {
        title: "Status project",
        description: "Inspect the current project, member scene files and startup scene.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["project_status"];
        if (override)
            return override({});
        return invoke("project.status", {});
    });
    server.registerTool("project_list", {
        title: "List project",
        description: "List available workspace project files.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["project_list"];
        if (override)
            return override({});
        return invoke("project.list", {});
    });
    server.registerTool("project_add_scene", {
        title: "Add Scene project",
        description: "Add an existing valid scene file to the current project; the first scene becomes its startup scene.",
        inputSchema: z.object({
            "sceneFile": z.string().min(1).max(128)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["project_add_scene"];
        if (override)
            return override(input);
        return invoke("project.add_scene", { "scene_file": input["sceneFile"] });
    });
    server.registerTool("project_remove_scene", {
        title: "Remove Scene project",
        description: "Remove scene membership without deleting its file; chooses a remaining startup scene when needed.",
        inputSchema: z.object({
            "sceneFile": z.string().min(1).max(128)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["project_remove_scene"];
        if (override)
            return override(input);
        return invoke("project.remove_scene", { "scene_file": input["sceneFile"] });
    });
    server.registerTool("project_set_startup", {
        title: "Set Startup project",
        description: "Choose a current project member as the startup scene.",
        inputSchema: z.object({
            "sceneFile": z.string().min(1).max(128)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["project_set_startup"];
        if (override)
            return override(input);
        return invoke("project.set_startup", { "scene_file": input["sceneFile"] });
    });
    server.registerTool("project_close", {
        title: "Close project",
        description: "Close project metadata while leaving the current scene intact.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["project_close"];
        if (override)
            return override({});
        return invoke("project.close", {});
    });
    server.registerTool("project_package", {
        title: "Package project",
        description: "Export saved project metadata, member scenes and non-hidden assets as a portable uncompressed tar under the project exports directory. Refuses to overwrite; save scene changes first.",
        inputSchema: z.object({
            "filename": z.string().min(5).max(128).regex(new RegExp("^[A-Za-z0-9][A-Za-z0-9._-]*\\.tar$"))
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["project_package"];
        if (override)
            return override(input);
        return invoke("project.package", { "filename": input["filename"] });
    });
    server.registerTool("scene_keyframe_set", {
        title: "Set transform keyframe",
        description: "Add or update a scene-owned transform keyframe at an exact time. Unspecified transform fields use the current entity transform or existing key. Undoable and saved with the scene.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "timeSeconds": z.number().finite().min(0).max(1000000),
            "px": z.number().finite().min(-1000000).max(1000000).optional(),
            "py": z.number().finite().min(-1000000).max(1000000).optional(),
            "pz": z.number().finite().min(-1000000).max(1000000).optional(),
            "rx": z.number().finite().min(-1000000).max(1000000).optional(),
            "ry": z.number().finite().min(-1000000).max(1000000).optional(),
            "rz": z.number().finite().min(-1000000).max(1000000).optional(),
            "sx": z.number().finite().min(-1000000).max(1000000).optional(),
            "sy": z.number().finite().min(-1000000).max(1000000).optional(),
            "sz": z.number().finite().min(-1000000).max(1000000).optional(),
            "gesture": z.number().int().min(0).max(4294967295).optional()
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_keyframe_set"];
        if (override)
            return override(input);
        return invoke("scene.keyframe.set", { "entity": input["entity"], "time_seconds": input["timeSeconds"], "px": input["px"], "py": input["py"], "pz": input["pz"], "rx": input["rx"], "ry": input["ry"], "rz": input["rz"], "sx": input["sx"], "sy": input["sy"], "sz": input["sz"], "gesture": input["gesture"] });
    });
    server.registerTool("scene_keyframe_delete", {
        title: "Delete transform keyframe",
        description: "Remove one scene-owned transform keyframe at an exact time as an undoable edit.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "timeSeconds": z.number().finite().min(0).max(1000000)
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_keyframe_delete"];
        if (override)
            return override(input);
        return invoke("scene.keyframe.delete", { "entity": input["entity"], "time_seconds": input["timeSeconds"] });
    });
    server.registerTool("scene_keyframes_playback", {
        title: "Control transform keyframes",
        description: "Set playback, loop, speed, duration or current time for scene-owned transform keys. Undoable and saved with the scene.",
        inputSchema: z.object({
            "entity": z.string().regex(new RegExp("^\\d+:\\d+$")),
            "playing": z.boolean().optional(),
            "loop": z.boolean().optional(),
            "speed": z.number().finite().min(-100).max(100).optional(),
            "durationSeconds": z.number().finite().min(0.001).max(1000000).optional(),
            "timeSeconds": z.number().finite().min(0).max(1000000).optional(),
            "gesture": z.number().int().min(0).max(4294967295).optional()
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_keyframes_playback"];
        if (override)
            return override(input);
        return invoke("scene.keyframes.playback", { "entity": input["entity"], "playing": input["playing"], "loop": input["loop"], "speed": input["speed"], "duration_seconds": input["durationSeconds"], "time_seconds": input["timeSeconds"], "gesture": input["gesture"] });
    });
    server.registerTool("scene_set_animations", {
        title: "Control animation tracks",
        description: "Seek or configure multiple animation roots in one undoable gesture; time clamps to each clip duration.",
        inputSchema: z.object({
            "entities": z.array(z.string().regex(new RegExp("^\\d+:\\d+$"))).min(1).max(4096),
            "playing": z.boolean().optional(),
            "loop": z.boolean().optional(),
            "speed": z.number().finite().min(-100).max(100).optional(),
            "timeSeconds": z.number().finite().min(0).max(1000000).optional(),
            "gesture": z.number().int().min(0).max(4294967295).optional().describe("Shared token for updates in one scrub gesture; zero creates a separate undo entry")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_animations"];
        if (override)
            return override(input);
        return invoke("scene.set_animations", { "entities": input["entities"], "playing": input["playing"], "loop": input["loop"], "speed": input["speed"], "time_seconds": input["timeSeconds"], "gesture": input["gesture"] });
    });
    server.registerTool("animation_clip", {
        title: "Inspect animation timeline",
        description: "Read bounded channel key times for an imported animation clip. Key times are read-only; truncated channels report their full key count.",
        inputSchema: z.object({
            "model": z.string().min(1).max(256),
            "clip": z.number().int().min(0).max(255)
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["animation_clip"];
        if (override)
            return override(input);
        return invoke("animation.clip", { "model": input["model"], "clip": input["clip"] });
    });
    server.registerTool("session_status", {
        title: "Inspect session grants",
        description: "Inspect exact native method grants for this connection. Grants are approved only by the host; no tool can expand them.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["session_status"];
        if (override)
            return override({});
        return invoke("session.status", {});
    });
    server.registerTool("session_audit", {
        title: "Inspect session audit",
        description: "Read bounded session action decisions and outcomes. Scope is the exact native method. Oldest sequence exposes eviction; parameters are not retained.",
        inputSchema: z.object({
            "after": z.number().int().min(0).default(0)
        }),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["session_audit"];
        if (override)
            return override(input);
        return invoke("session.audit", { "after": input["after"] });
    });
    server.registerTool("session_request", {
        title: "Session request",
        description: "Request host approval for an exact method and optional entity/file target. This never grants access or executes an action.",
        inputSchema: z.object({
            "scope": z.string().max(64),
            "kind": z.enum(["method", "entity", "file"]).default("method"),
            "target": z.string().max(128).default("")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["session_request"];
        if (override)
            return override(input);
        return invoke("session.request", { "scope": input["scope"], "kind": input["kind"], "target": input["target"] });
    });
}
//# sourceMappingURL=generated_protocol.js.map