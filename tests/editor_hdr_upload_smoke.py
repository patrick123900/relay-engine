#!/usr/bin/env python3
"""Real desktop Vulkan regression for exposure, transform keys, uploads and project export."""

import os
import shutil
import tarfile
import time
from pathlib import Path

from PIL import Image

from editor_interaction_smoke import Editor, ROOT


def mean_center(path):
    with Image.open(path) as image:
        rgb = image.convert("RGB")
        width, height = rgb.size
        area = rgb.crop((width * 2 // 5, height * 2 // 5,
                         width * 3 // 5, height * 3 // 5))
        values = area.get_flattened_data()
        return sum(sum(pixel) for pixel in values) / (3 * area.width * area.height)


def main():
    project_root = Path(ROOT) / "projects" / f"hdr-upload-smoke-{os.getpid()}"
    names = ("hdr-zero.png", "hdr-bright.png", "key-left.png", "key-right.png")
    editor = Editor()
    try:
        editor.result("runtime.pause")
        status = editor.result("render.upload_status")
        assert status["available"] and status["submitted_batches"] >= 1, status
        assert status["estimated_device_bytes"] > 0 and status["rejected_batches"] == 0, status
        print(f"PASS: live upload telemetry reports {status['submitted_batches']} batches, "
              f"dedicated transfer queue={status['dedicated_transfer_queue']}", flush=True)

        project_file = f"projects/{project_root.name}/project.relayproject"
        editor.result("project.create", filename=project_file, name="HDR upload smoke")
        quad = editor.result("scene.create", name="Animated quad")["entity"]
        editor.result("scene.set_renderer", entity=quad, mesh="builtin.quad",
                      material="builtin.azure")
        camera = editor.result("scene.create", name="Exposure camera")["entity"]
        editor.result("scene.set_transform", entity=camera, pz=5)
        editor.result("scene.set_camera", entity=camera, active=True, exposure_ev=0)
        editor.result("editor.camera.set", mode="scene")
        time.sleep(1)
        editor.result("render.capture", path=names[0], source="vulkan")
        editor.result("scene.set_camera", entity=camera, exposure_ev=2)
        time.sleep(.5)
        editor.result("render.capture", path=names[1], source="vulkan")
        base = mean_center(Path(ROOT) / "captures" / names[0])
        bright = mean_center(Path(ROOT) / "captures" / names[1])
        assert bright > base + 5, (base, bright)
        print(f"PASS: camera exposure brightens live Vulkan capture ({base:.1f} to {bright:.1f})",
              flush=True)

        editor.result("scene.set_camera", entity=camera, exposure_ev=0)
        editor.result("scene.keyframe.set", entity=quad, time_seconds=0, px=-1)
        editor.result("scene.keyframe.set", entity=quad, time_seconds=1, px=1)
        editor.result("scene.keyframes.playback", entity=quad, time_seconds=0, playing=False)
        time.sleep(.5)
        editor.result("render.capture", path=names[2], source="vulkan")
        editor.result("scene.keyframes.playback", entity=quad, time_seconds=1, playing=False)
        time.sleep(.5)
        editor.result("render.capture", path=names[3], source="vulkan")
        with Image.open(Path(ROOT) / "captures" / names[2]) as left, \
             Image.open(Path(ROOT) / "captures" / names[3]) as right:
            assert left.size == right.size and left.tobytes() != right.tobytes()
        print("PASS: scene-owned transform keys change the live Vulkan image", flush=True)

        shutil.copyfile(Path(ROOT) / "tests/fixtures/models/relay-dynamic-golden.glb",
                        project_root / "model.glb")
        editor.result("assets.import_model", filename="model.glb")
        time.sleep(1)
        after_import = editor.result("render.upload_status")
        assert (after_import["submitted_batches"] > status["submitted_batches"] and
                after_import["rejected_batches"] == 0), (status, after_import)
        print("PASS: importing a model submits an additional bounded GPU upload", flush=True)

        editor.result("scene.save", filename="animated.relay.json")
        editor.result("project.add_scene", scene_file="animated.relay.json")
        editor.result("project.package", filename="portable.tar")
        archive = project_root / "exports/portable.tar"
        with tarfile.open(archive) as packaged:
            files = set(packaged.getnames())
            assert {"project.relayproject", "scenes/animated.relay.json", "model.glb",
                    ".relay-imports.json"} <= files, files
            scene = packaged.extractfile("scenes/animated.relay.json").read()
            assert b'"transform_animation"' in scene
        print("PASS: live project export is readable and includes saved transform keys", flush=True)
    finally:
        editor.close()
        for name in names:
            (Path(ROOT) / "captures" / name).unlink(missing_ok=True)
        # The generated fixture is isolated and was created by this test.
        shutil.rmtree(project_root, ignore_errors=True)


if __name__ == "__main__":
    main()
