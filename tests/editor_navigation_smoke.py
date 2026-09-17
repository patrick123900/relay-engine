#!/usr/bin/env python3
"""Real-input Godot-style navigation and imported light gizmo regression checks.

Requires the same Linux desktop/xdotool setup as editor_interaction_smoke.py.
"""
import hashlib
from pathlib import Path
import time
from editor_interaction_smoke import Editor, tree_row, xdo, ROOT, VIEWPORT_CENTRE


def capture(editor, name):
    path = f"captures/{name}.png"
    editor.result("render.capture", path=path, source="vulkan")
    return hashlib.sha256((Path(ROOT) / path).read_bytes()).hexdigest()


def drag_view(editor, button, dx, dy, shift=False):
    editor.move(*VIEWPORT_CENTRE)
    if shift:
        xdo("keydown", "Shift_L")
    xdo("mousedown", str(button))
    time.sleep(.15)
    for step in range(1, 7):
        if button == 3:
            xdo("mousemove_relative", "--", str(dx // 6), str(dy // 6))
            time.sleep(.08)
            continue
        editor.move(VIEWPORT_CENTRE[0] + dx * step // 6,
                    VIEWPORT_CENTRE[1] + dy * step // 6)
    xdo("mouseup", str(button))
    if shift:
        xdo("keyup", "Shift_L")
    time.sleep(.4)


def main():
    editor = Editor()
    try:
        assert editor.calibrate()
        editor.result("runtime.pause")
        editor.result("assets.import_model", filename="relay-dynamic-golden.gltf")
        time.sleep(1)
        entities = editor.result("scene.list")["entities"]
        light = next(e for e in entities if e["name"] == "Sun Light")
        editor.click(110, tree_row(entities.index(light)))
        editor.key("e")
        editor.key("f")
        editor.screenshot("phase-f-light-focus.png")
        before = capture(editor, "phase-f-navigation-before")
        scene = editor.result("scene.list")["entities"]
        history = editor.result("scene.history")
        editor.result("trace.start", filename="editor-navigation.relay-trace.jsonl")
        drag_view(editor, 2, 60, 20)
        drag_view(editor, 2, 45, 15, shift=True)
        drag_view(editor, 3, 30, 10)
        editor.move(*VIEWPORT_CENTRE)
        xdo("mousedown", "3")
        xdo("keydown", "w")
        time.sleep(.4)
        xdo("keyup", "w")
        xdo("mouseup", "3")
        time.sleep(.4)
        editor.move(*VIEWPORT_CENTRE)
        editor.key("shift+f")
        xdo("keydown", "d")
        time.sleep(.25)
        xdo("keyup", "d")
        editor.key("Escape")
        stopped = editor.result("trace.stop")
        assert stopped["events"] == 0, stopped
        assert editor.result("scene.list")["entities"] == scene
        assert editor.result("scene.history") == history
        after = capture(editor, "phase-f-navigation-after")
        assert before != after, "viewport navigation did not change the Vulkan image"
        print("PASS: orbit, Shift-pan and RMB/W freelook change the view without scene/history/trace mutations")
        editor.key("f")
        editor.screenshot("phase-f-light-refocus.png")
        editor.key("w")
        # Framing the empty light keeps the origin at the center and a usable fixed screen-size
        # gizmo. A center drag must move the selected light, rather than another object or the view.
        original = editor.result("scene.inspect", entity=light["entity"])["transform"]
        depth = len(editor.result("scene.history")["undo"])
        editor.drag(*VIEWPORT_CENTRE, VIEWPORT_CENTRE[0]+60, VIEWPORT_CENTRE[1])
        moved = editor.result("scene.inspect", entity=light["entity"])["transform"]
        assert moved != original, (original, moved)
        assert moved["rotation_degrees"] == original["rotation_degrees"] and moved["scale"] == original["scale"], moved
        assert len(editor.result("scene.history")["undo"]) == depth + 1
        editor.key("ctrl+z")
        assert editor.result("scene.inspect", entity=light["entity"])["transform"] == original, (editor.result("scene.inspect", entity=light["entity"])["transform"], original, editor.result("scene.history"))
        print("PASS: imported Sun Light focuses at usable distance; gizmo drag is one reversible edit")
        editor.result("scene.set_transform", entity=light["parent"], sx=.01, sy=.02, sz=.01)
        time.sleep(.8)
        editor.key("f")
        depth = len(editor.result("scene.history")["undo"])
        editor.drag(*VIEWPORT_CENTRE, VIEWPORT_CENTRE[0]+60, VIEWPORT_CENTRE[1])
        scaled = editor.result("scene.inspect", entity=light["entity"])["transform"]
        assert scaled != original and scaled["scale"] == original["scale"] and scaled["rotation_degrees"] == original["rotation_degrees"], scaled
        assert len(editor.result("scene.history")["undo"]) == depth + 1
        editor.screenshot("phase-f-light-scaled-parent.png")
        editor.key("ctrl+z")
        assert editor.result("scene.inspect", entity=light["entity"])["transform"] == original, (editor.result("scene.inspect", entity=light["entity"])["transform"], original, editor.result("scene.history"))
        print("PASS: light under a small nonuniform parent retains usable gizmo sizing and local edit/undo")
    finally:
        xdo("keyup", "w", "Shift_L")
        xdo("mouseup", "2", "3")
        editor.close()


if __name__ == "__main__":
    main()
