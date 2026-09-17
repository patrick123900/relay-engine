import { z } from "zod";
export function registerGeneratedTools(server, invoke, overrides = {}) {
    server.registerTool("runtime_status", {
        title: "Inspect Relay runtime",
        description: "Read the current run, pause, frame, simulation time and resolution state.",
        inputSchema: z.object({}),
        annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
    }, async () => {
        const override = overrides["runtime_status"];
        if (override)
            return override({});
        return invoke("runtime.status", {});
    });
    server.registerTool("runtime_pause", {
        title: "Pause Relay runtime",
        description: "Pause automatic simulation so the scene can be inspected deterministically.",
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
        description: "Resume automatic simulation after an inspection or controlled frame step.",
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
        description: "Advance an exact number of deterministic simulation frames, including while paused.",
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
            "filename": z.string().min(5).max(128).regex(new RegExp("^[A-Za-z0-9][A-Za-z0-9._-]*\\.(gltf|glb|fbx|obj|dae|blend)$")).describe("Safe project-local model filename without directory components"),
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
            "orthographicHeight": z.number().finite().min(0).max(1000000).optional().describe("Full viewport height; zero selects perspective")
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_camera"];
        if (override)
            return override(input);
        return invoke("scene.set_camera", { "entity": input["entity"], "enabled": input["enabled"], "active": input["active"], "field_of_view_y_degrees": input["fieldOfViewY"], "near_plane": input["nearPlane"], "far_plane": input["farPlane"], "orthographic_height": input["orthographicHeight"] });
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
            "timeSeconds": z.number().finite().min(0).max(1000000).optional()
        }),
        annotations: { readOnlyHint: false, destructiveHint: false, openWorldHint: false },
    }, async (input) => {
        const override = overrides["scene_set_animation"];
        if (override)
            return override(input);
        return invoke("scene.set_animation", { "entity": input["entity"], "clip": input["clip"], "playing": input["playing"], "loop": input["loop"], "speed": input["speed"], "time_seconds": input["timeSeconds"] });
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
}
//# sourceMappingURL=generated_protocol.js.map