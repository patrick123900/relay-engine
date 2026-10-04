#!/usr/bin/env python3
"""Real desktop check of the template editor window: opens the demo's Ball template, edits it with the
gizmo, undoes, saves, and confirms the viewport shows a GPU-rendered picture.

Requires a desktop session, xdotool and spectacle, like the other editor smokes, and restores the
template file it saves. Coordinates are derived from the window's layout at its first open.

    python3 tests/editor_template_smoke.py
"""
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import editor_interaction_smoke as smoke

ROOT = smoke.ROOT
TEMPLATE = Path(ROOT) / "examples/demo/templates/Ball.relay-template.json"


def windows(editor):
    return editor.call("editor.template.status")["result"]["windows"]


def wait_for(check, seconds=8.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if check():
            return True
        time.sleep(0.2)
    return False


def main():
    original = TEMPLATE.read_bytes()
    popen = subprocess.Popen

    def with_demo(args, **kwargs):
        if "env" in kwargs:
            kwargs["env"] = dict(kwargs["env"], RELAY_OPEN_DEMO_PROJECT="1")
        return popen(args, **kwargs)

    smoke.subprocess.Popen = with_demo
    editor = smoke.Editor()
    smoke.subprocess.Popen = popen
    try:
        time.sleep(3)
        smoke.check(editor.call("editor.template.open", name="Missing")["ok"] is False, "a missing template does not open")
        reply = editor.call("editor.template.open", name="Ball")
        smoke.check(reply["ok"] and reply["result"]["windows"][0]["name"] == "Ball", "the Ball template opens in a window")
        smoke.check(wait_for(lambda: windows(editor)[0]["gpu"]), "the viewport shows a GPU-rendered picture")
        smoke.check(not windows(editor)[0]["dirty"], "a freshly opened template has no unsaved edits")
        shot = editor.screenshot("template-window.png")
        smoke.check(Path(shot).exists(), "a screenshot of the window was taken")
        # The layout is the window's default one: hierarchy left, gizmo in the middle of the viewport.
        # These are client coordinates found from a screenshot of that layout.
        row, gizmo_up, save = (124, 192), (553, 391), (97, 124)
        editor.click(*row)
        editor.drag(*gizmo_up, gizmo_up[0], gizmo_up[1] - 60)
        smoke.check(wait_for(lambda: windows(editor)[0]["dirty"]), "dragging the gizmo edits the template")
        editor.key("ctrl+z")
        smoke.check(wait_for(lambda: not windows(editor)[0]["dirty"]), "Ctrl+Z undoes it inside the window")
        editor.drag(*gizmo_up, gizmo_up[0], gizmo_up[1] - 60)
        smoke.check(wait_for(lambda: windows(editor)[0]["dirty"]), "the edit can be made again")
        editor.click(*save)
        smoke.check(wait_for(lambda: not windows(editor)[0]["dirty"]), "Save clears the unsaved mark")
        smoke.check(TEMPLATE.read_bytes() != original, "Save wrote the template file")
        # Dragging the window's corner changes the picture's size every frame; each size used to be a
        # new ImGui texture, which ran the backend's descriptor pool dry and crashed the editor.
        import math
        corner = (889 * 1.38 - 97, 621 * 1.38 - 109)
        x0, y0 = editor.origin[0] + corner[0], editor.origin[1] + corner[1]
        smoke.xdo("mousemove", str(int(x0)), str(int(y0)))
        time.sleep(0.3)
        smoke.xdo("mousedown", "1")
        survived = True
        for step in range(600):
            smoke.xdo("mousemove", str(int(x0 + 150 + 140 * math.sin(step / 7.0))), str(int(y0 + 110 + 90 * math.sin(step / 5.0))))
            if editor.process.poll() is not None:
                survived = False
                break
        smoke.xdo("mouseup", "1")
        smoke.check(survived and editor.process.poll() is None, "the editor survives resizing the window continuously")
    finally:
        editor.close()
        TEMPLATE.write_bytes(original)
    sys.exit(1 if smoke.FAILURES else 0)


if __name__ == "__main__":
    main()
