#!/usr/bin/env python3
"""Real desktop/GPU regression for the directional shadow pass.

Run from the repository root: python3 tests/editor_shadow_smoke.py
"""
import time
from pathlib import Path

from PIL import Image

from editor_interaction_smoke import Editor


def main():
    editor = Editor()
    try:
        editor.result("runtime.pause")
        receiver = editor.result("scene.create", name="Shadow receiver")["entity"]
        editor.result("scene.set_renderer", entity=receiver, enabled=True,
                      mesh="builtin.quad", material="builtin.orange")
        editor.result("scene.set_transform", entity=receiver, sx=4, sy=4)

        caster = editor.result("scene.create", name="Shadow caster")["entity"]
        editor.result("scene.set_renderer", entity=caster, enabled=True,
                      mesh="builtin.quad", material="builtin.azure")
        editor.result("scene.set_transform", entity=caster, py=-.7, pz=1, sx=.7, sy=.7)

        light = editor.result("scene.create", name="Shadow light")["entity"]
        editor.result("scene.set_transform", entity=light, rx=45)
        editor.result("scene.set_light", entity=light, enabled=True, type="directional",
                      intensity=3, red=1, green=1, blue=1)
        time.sleep(2)

        shadow_path = Path("captures/shadow-live-caster.png")
        clear_path = Path("captures/shadow-live-clear.png")
        editor.result("render.capture", path=str(shadow_path), source="vulkan")
        editor.result("scene.set_transform", entity=caster, px=20)
        time.sleep(1)
        editor.result("render.capture", path=str(clear_path), source="vulkan")

        with Image.open(shadow_path) as image:
            shadow = image.convert("RGB").tobytes()
        with Image.open(clear_path) as image:
            clear = image.convert("RGB").tobytes()
        assert len(shadow) == len(clear)
        shadow_pixels = zip(shadow[0::3], shadow[1::3], shadow[2::3])
        clear_pixels = zip(clear[0::3], clear[1::3], clear[2::3])
        darker = sum(sum(reference) - sum(cast) > 45
                     for cast, reference in zip(shadow_pixels, clear_pixels))
        assert darker > 250, f"directional shadow absent or too small: {darker} darker pixels"
        print(f"PASS: live Vulkan directional shadow contributes {darker} darker pixels")
    finally:
        editor.close()


if __name__ == "__main__":
    main()
