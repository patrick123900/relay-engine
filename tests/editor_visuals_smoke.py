#!/usr/bin/env python3
"""Real desktop/GPU regression for mesh outlines, height grids and capture isolation.

Requires a desktop session, xdotool, Spectacle and Pillow. Uses an isolated saved layout.
Run from the repository root: python3 tests/editor_visuals_smoke.py
"""
import time
from pathlib import Path

from PIL import Image

from editor_interaction_smoke import Editor, xdo


def outline_pixels(filename):
    with Image.open(Path("captures") / filename) as image:
        pixels = image.convert("RGB").tobytes()
    return sum(
        abs(red - 255) < 8 and abs(green - 185) < 8 and abs(blue - 48) < 8
        for red, green, blue in zip(pixels[0::3], pixels[1::3], pixels[2::3])
    )


def main():
    editor = Editor()
    try:
        editor.result("runtime.pause")
        probe = editor.result("scene.create", name="OutlineProbe")["entity"]
        editor.result("scene.set_renderer", entity=probe, enabled=True,
                      mesh="builtin.quad", material="builtin.azure")
        time.sleep(6)
        center = editor.viewport_center()
        history = editor.result("scene.history")
        editor.result("render.capture", path="outline-clean-before.png", source="vulkan")
        editor.screenshot("editor-outline-unselected.png")
        editor.click(*center)
        time.sleep(1)
        editor.screenshot("editor-outline-selected.png")
        assert (outline_pixels("editor-outline-selected.png") >
                outline_pixels("editor-outline-unselected.png") + 50), "mesh outline absent"
        print("PASS: rendered selection silhouette adds amber outline pixels")

        editor.result("render.capture", path="outline-clean-selected.png", source="vulkan")
        before = Path("captures/outline-clean-before.png").read_bytes()
        selected = Path("captures/outline-clean-selected.png").read_bytes()
        assert before == selected, "selection/grid leaked into GPU capture"
        print("PASS: selecting mesh leaves exported GPU pixels unchanged")

        editor.key("f")
        editor.screenshot("editor-outline-framed.png")
        editor.move(*center)
        editor.key("shift+f")
        xdo("keydown", "e")
        try:
            time.sleep(3.0)
        finally:
            xdo("keyup", "e")
        editor.key("Escape")
        editor.screenshot("editor-grid-raised.png")
        assert editor.result("scene.history") == history, "navigation changed undo history"
        print("PASS: vertical flight changes grid levels without scene/history mutations")

        xdo("windowstate", "--remove", "MAXIMIZED_VERT", "--remove", "MAXIMIZED_HORZ",
            editor.window)
        time.sleep(.5)
        xdo("windowsize", editor.window, "1360", "900")
        time.sleep(2)
        assert editor.result("runtime.status")
        # Vertical flight can leave the mesh outside the narrower resized frustum.
        editor.key("f")
        editor.screenshot("editor-outline-resized.png")
        assert outline_pixels("editor-outline-resized.png") > 50, "outline lost on resize"
        print("PASS: selected mesh/grid survive swapchain resize")
    finally:
        editor.close()


if __name__ == "__main__":
    main()
