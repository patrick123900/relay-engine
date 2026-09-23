"""Run inside Blender to round-trip Relay's dynamic golden through Blender/GLB/FBX.

blender --background --factory-startup --python tools/generate_phase_d_fixtures.py -- OUTPUT_DIR
The original authored glTF is the stable test oracle; these files exercise real adapter backends.
"""
import pathlib
import sys
import os

import bpy

destination = pathlib.Path(sys.argv[sys.argv.index("--") + 1]).resolve()
destination.mkdir(parents=True, exist_ok=True)
source = pathlib.Path(__file__).resolve().parent.parent / "tests" / "fixtures" / "models" / "relay-dynamic-golden.gltf"
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)
bpy.context.preferences.filepaths.save_version = 0
bpy.ops.import_scene.gltf(filepath=str(source))
bpy.ops.wm.save_as_mainfile(filepath=str(destination / "relay-dynamic-golden.blend"))
bpy.ops.export_scene.gltf(filepath=str(destination / "relay-dynamic-golden.glb"),
                          export_format="GLB", export_animations=True,
                          export_cameras=True, export_lights=True)
bpy.ops.export_scene.fbx(filepath=str(destination / "relay-dynamic-golden.fbx"),
                         use_selection=False, add_leaf_bones=False,
                         bake_anim=True, path_mode="COPY", embed_textures=True)
# Explicit process exit avoids audio-backend shutdown hangs in restricted/headless environments.
sys.stdout.flush()
sys.stderr.flush()
os._exit(0)
