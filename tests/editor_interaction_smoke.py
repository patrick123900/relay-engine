#!/usr/bin/env python3
"""Drive the Relay editor with real mouse and keyboard input.

Every interaction is checked through the stdio protocol channel rather than by eyeballing a
screenshot, so the assertions are about engine state: did clicking that row really select it, did
the gizmo drag really produce one undo entry, did Ctrl+Z really rewind the whole gesture.

Requires a desktop session, xdotool, and SDL forced onto XWayland (done below). Some desktops gate
synthetic input behind a Remote Control permission prompt that must be granted once. The compositor
may also scale pointer coordinates, so the pointer is driven in a closed loop until it reports the
position we want rather than assuming a requested position is where it lands.

    python3 tests/editor_interaction_smoke.py
"""
import json
import os
import re
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time

ROOT = str(Path(__file__).resolve().parents[1])
FAILURES = []

# Client-space layout, derived from the constants in editor_ui.cpp's build() and ImGui's metrics
# (frame height = font size + 2 * frame padding). Calibration below fails loudly if the toolbar
# moves, which is the signal that these need recomputing after a restyle.
PANEL_SIDE = 269           # max(268, width * 0.21)
TOOLBAR_HEIGHT = 47        # frame height (15 + 2*5) + 22
BOTTOM_HEIGHT = 214        # max(214, height * 0.28)
TREE_FIRST_ROW_Y = 248     # panel padding + header block + create row
TREE_ROW_HEIGHT = 22       # text line height + item spacing
ASSET_FIRST_ROW = (100, 616)
VIEWPORT_CENTRE = (640, 316)


def tree_row(index):
    return TREE_FIRST_ROW_Y + TREE_ROW_HEIGHT * index


def check(condition, message):
    print(("  PASS  " if condition else "  FAIL  ") + message)
    if not condition:
        FAILURES.append(message)


def xdo(*args):
    return subprocess.run(["xdotool", *args], capture_output=True, text=True, timeout=10).stdout.strip()


class Editor:
    def __init__(self, layout_path=None):
        self.layout_directory = tempfile.TemporaryDirectory(prefix="relay-editor-layout-") if layout_path is None else None
        self.layout_path = Path(layout_path) if layout_path is not None else Path(self.layout_directory.name) / "layout.ini"
        env = dict(os.environ, SDL_VIDEODRIVER="x11")
        env["RELAY_EDITOR_LAYOUT_PATH"] = str(self.layout_path)
        self.process = subprocess.Popen(
            ["./build/dev/relay_demo", "--editor-ui-stdio"],
            cwd=ROOT, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
        )
        if not select.select([self.process.stdout], [], [], 20)[0]:
            self.process.kill()
            raise TimeoutError("editor startup timed out")
        ready = self.process.stdout.readline()
        if "relay.ready" not in ready:
            raise RuntimeError("editor did not start: " + ready)
        self.next_id = 1
        time.sleep(1.5)
        # The editor puts the scene name and its unsaved marker in front of "Relay Editor", so the
        # match is anchored on the trailing name rather than the whole title.
        self.window = xdo("search", "--name", "Relay Editor$").split("\n")[-1]
        if not self.window:
            raise RuntimeError("could not find the editor window")
        # Avoid unreachable targets at mixed-scale monitor seams, especially compact icons.
        xdo("windowmove", self.window, "100", "60")
        xdo("windowactivate", "--sync", self.window)
        time.sleep(0.5)
        geometry = dict(
            line.split("=")
            for line in xdo("getwindowgeometry", "--shell", self.window).split("\n")
            if "=" in line
        )
        self.origin = (int(geometry["X"]), int(geometry["Y"]))
        # xdotool reports the frame origin, which sits above the client area by the title bar
        # height. Solve for that offset by probing the Pause button rather than assuming it.
        self.offset = (0, 0)
        self.pause_button = None
        print(f"window {self.window} at {self.origin} size {geometry['WIDTH']}x{geometry['HEIGHT']}")

    def call(self, method, **params):
        request = {"id": self.next_id, "method": method}
        request.update(params)
        self.next_id += 1
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        if not select.select([self.process.stdout], [], [], 20)[0]:
            raise TimeoutError(f"editor request timed out: {method}")
        return json.loads(self.process.stdout.readline())

    def result(self, method, **params):
        response = self.call(method, **params)
        if not response.get("ok"):
            raise RuntimeError(f"{method} failed: {response.get('error')}")
        return response["result"]

    @staticmethod
    def pointer():
        location = dict(
            part.split(":") for part in xdo("getmouselocation").split(" ") if ":" in part
        )
        return int(location["x"]), int(location["y"])

    # This compositor scales pointer coordinates, so a requested position is not where the pointer
    # lands. Rather than model the scale, drive the pointer in a closed loop until it reports the
    # position we actually want. Coordinates are client-relative to the editor window.
    def move(self, x, y):
        target = (self.origin[0] + x, self.origin[1] + y)
        request = target
        for _ in range(32):
            if max(abs(a-b) for a,b in zip(self.pointer(), target)) <= 1:
                return True
            xdo("mousemove", str(request[0]), str(request[1]))
            time.sleep(.04)
            actual = self.pointer()
            if max(abs(a-b) for a,b in zip(actual, target)) <= 1:
                return True
            # Half-gain corrections converge on 2x-scaled monitors rather than oscillating
            # between equally distant positions on each side of the target.
            request = (round(request[0] + .5 * (target[0] - actual[0])),
                       round(request[1] + .5 * (target[1] - actual[1])))
        # Some XWayland/portal paths stop accepting absolute warps after a capture release.
        # Relative warps avoid that coordinate mapping and still converge from observed positions.
        for _ in range(16):
            actual = self.pointer()
            if max(abs(a-b) for a,b in zip(actual,target)) <= 1: return True
            xdo("mousemove_relative", "--", str(round(.5*(target[0]-actual[0]))),
                str(round(.5*(target[1]-actual[1]))))
            time.sleep(.04)
        raise RuntimeError(f"pointer did not reach {target}; actual={self.pointer()}")

    def panel_rect(self, name):
        document = self.layout_path.read_text()
        section = re.search(r"\[Window\]\[" + re.escape(name) + r"\]\n(.*?)(?=\n\[|\Z)", document, re.S)
        if not section:
            raise RuntimeError(f"missing saved panel geometry: {name}")
        values = dict(line.split("=",1) for line in section.group(1).splitlines() if "=" in line)
        return tuple(int(v) for key in ("Pos", "Size") for v in values[key].split(","))

    def viewport_center(self):
        x,y,width,height = self.panel_rect("Viewport")
        return (x + width//2, y + (height+26)//2)

    def calibrate(self):
        """Locate the toolbar Pause button, which also validates the whole coordinate path.

        Scanning rather than hardcoding keeps the harness working when the toolbar is restyled or
        the font metrics change. Anything else in the scanned range is harmless to click.
        """
        deadline = time.monotonic() + 8
        while not self.layout_path.exists() and time.monotonic() < deadline:
            time.sleep(.1)
        # The toolbar is an unsaved viewport side bar, so recent ImGui layouts no longer
        # contain a [Window][Controls] entry. Its buttons remain directly below the menu.
        button_y = 49
        for y in (button_y, button_y-6, button_y+6):
            for x in range(20, 230, 10):
                before = self.result("runtime.status")["paused"]
                self.click(x, y)
                if self.result("runtime.status")["paused"] != before:
                    self.click(x, y)
                    self.pause_button = (x, y)
                    print(f"   found Pause at client {self.pause_button}")
                    return True
        return False

    def click(self, x, y, button="1"):
        self.move(x, y)
        time.sleep(0.12)
        xdo("click", button)
        time.sleep(0.35)

    def double_click(self, x, y):
        self.move(x, y)
        time.sleep(0.12)
        xdo("click", "--repeat", "2", "--delay", "40", "1")
        time.sleep(0.6)

    def drag(self, x0, y0, x1, y1, steps=10):
        self.move(x0, y0)
        time.sleep(0.15)
        xdo("mousedown", "1")
        time.sleep(0.15)
        for step in range(1, steps + 1):
            self.move(int(x0 + (x1 - x0) * step / steps), int(y0 + (y1 - y0) * step / steps))
            time.sleep(0.03)
        time.sleep(0.15)
        xdo("mouseup", "1")
        time.sleep(0.4)

    def key(self, *keys):
        for k in keys:
            xdo("key", "--window", self.window, k)
            time.sleep(0.3)

    def type_text(self, text):
        xdo("type", "--delay", "40", text)
        time.sleep(0.3)

    def screenshot(self, name):
        path = os.path.join(ROOT, "captures", name)
        xdo("windowactivate", "--sync", self.window)
        time.sleep(0.4)
        subprocess.run(["spectacle", "-a", "-b", "-n", "-o", path], timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Screenshot tools may take keyboard focus while capturing. Restore the editor before
        # subsequent synthetic keyboard input rather than relying on compositor timing.
        xdo("windowactivate", "--sync", self.window)
        time.sleep(.3)
        return path

    def close(self):
        try:
            self.call("runtime.quit")
            self.process.wait(timeout=15)
        except Exception:
            self.process.kill()
            self.process.wait(timeout=5)
        if self.layout_directory:
            self.layout_directory.cleanup()


def main():
    editor = Editor()
    try:
        print("\n[calibrate] toolbar")
        calibrated = editor.calibrate()
        check(calibrated, "clicking the toolbar Pause button toggles the runtime")
        if not calibrated:
            editor.screenshot("phase-f-calibration.png")
            raise RuntimeError("coordinate calibration failed; aborting")

        # --- Asset browser: double-clicking a listed model imports it. ---
        print("\n[assets] import by double-click")
        before = len(editor.result("scene.list")["entities"])
        editor.double_click(*ASSET_FIRST_ROW)
        time.sleep(2.0)
        entities = editor.result("scene.list")["entities"]
        check(len(entities) > before,
              f"double-clicking a model in the asset browser imported it ({before} -> {len(entities)})")
        print("   entities:", [e["name"] for e in entities])

        # Importing selects and frames the new root, so the inspector should now be populated.
        editor.screenshot("phase-f-inspector.png")

        # --- Gizmo drag. A quad at the world origin frames to the viewport centre, so the gizmo
        # lands in a known place instead of wherever the imported model happens to sit. ---
        print("\n[gizmo] drag translate")
        probe = editor.result("scene.create", name="GizmoProbe")["entity"]
        editor.result("scene.set_renderer", entity=probe, enabled=True,
                      mesh="builtin.quad", material="builtin.azure")
        editor.result("scene.set_transform", entity=probe, px=0, py=0, pz=0)
        time.sleep(0.8)
        # Select it by clicking its hierarchy row, then frame it with F.
        entities = editor.result("scene.list")["entities"]
        editor.click(110, tree_row([e["entity"] for e in entities].index(probe)))
        editor.key("f")
        time.sleep(0.6)
        editor.key("w")
        editor.screenshot("phase-f-gizmo-before.png")

        depth_before = len(editor.result("scene.history")["undo"])
        position_before = editor.result("scene.inspect", entity=probe)["transform"]["position"]
        editor.drag(VIEWPORT_CENTRE[0], VIEWPORT_CENTRE[1],
                    VIEWPORT_CENTRE[0] + 120, VIEWPORT_CENTRE[1])
        history = editor.result("scene.history")["undo"]
        position_after = editor.result("scene.inspect", entity=probe)["transform"]["position"]
        added = len(history) - depth_before
        moved = position_after != position_before
        check(moved, f"dragging the gizmo moved the entity ({position_before} -> {position_after})")
        check(added == 1 and history[0].startswith("Transform"),
              f"a whole gizmo drag is exactly one undo entry (added {added}, top={history[:1]})")
        editor.screenshot("phase-f-gizmo.png")

        if moved:
            print("\n[undo] ctrl+z rewinds the whole gesture")
            editor.key("ctrl+z")
            restored = editor.result("scene.inspect", entity=probe)["transform"]["position"]
            check(restored == position_before,
                  f"Ctrl+Z rewound the entire drag, not just its last update ({restored})")

        # --- Gizmo mode shortcuts. ---
        print("\n[gizmo] rotate and scale modes")
        editor.key("e")
        editor.screenshot("phase-f-gizmo-rotate.png")
        editor.key("r")
        editor.screenshot("phase-f-gizmo-scale.png")
        editor.key("w")

        # --- Hierarchy click selects: proven by deleting the selection afterwards. ---
        print("\n[hierarchy] click selects, Delete destroys the selection")
        entities = editor.result("scene.list")["entities"]
        target = entities[1] if len(entities) > 1 else entities[0]
        editor.click(110, tree_row(entities.index(target)))
        editor.key("Delete")
        remaining = {e["entity"] for e in editor.result("scene.list")["entities"]}
        check(target["entity"] not in remaining,
              f"clicking row '{target['name']}' selected it and Delete destroyed it")
        editor.key("ctrl+z")

        # --- Drag and drop reparenting. ---
        print("\n[hierarchy] drag and drop reparenting")
        entities = editor.result("scene.list")["entities"]
        roots = [e for e in entities if e["parent"] is None]
        if len(roots) >= 2:
            source, destination = roots[0], roots[1]
            editor.drag(110, tree_row(entities.index(source)),
                        110, tree_row(entities.index(destination)), steps=12)
            updated = {e["entity"]: e["parent"] for e in editor.result("scene.list")["entities"]}
            check(updated.get(source["entity"]) == destination["entity"],
                  f"dragging '{source['name']}' onto '{destination['name']}' reparented it "
                  f"(parent now {updated.get(source['entity'])})")
        else:
            check(False, "needed two root entities to test drag and drop reparenting")

        # --- Context menu. Dismissed by clicking empty hierarchy space, never with Escape,
        # which closes the window. ---
        print("\n[hierarchy] context menu")
        editor.click(110, tree_row(0), button="3")
        editor.screenshot("phase-f-context-menu.png")
        editor.click(200, 430)  # empty hierarchy space dismisses the popup

        # --- Viewport click-to-select. ---
        print("\n[viewport] click to pick")
        entities = editor.result("scene.list")["entities"]
        drawables = [e for e in entities if e.get("mesh_renderer")]
        if drawables:
            editor.click(*VIEWPORT_CENTRE)
            editor.key("Delete")
            remaining = {e["entity"] for e in editor.result("scene.list")["entities"]}
            picked = [d for d in drawables if d["entity"] not in remaining]
            check(bool(picked),
                  f"clicking an object in the viewport selected it ({[d['name'] for d in picked]})")
            editor.key("ctrl+z")
        else:
            check(False, "scene had no drawable entity to pick")

        # --- Camera navigation must change the view without touching the scene. ---
        print("\n[camera] orbit does not mutate the scene")
        history_before = editor.result("scene.history")["undo"]
        snapshot_before = editor.result("scene.list")["entities"]
        editor.move(*VIEWPORT_CENTRE)
        xdo("mousedown", "2")
        for step in range(10):
            editor.move(VIEWPORT_CENTRE[0] + step * 8, VIEWPORT_CENTRE[1] + step * 3)
        xdo("mouseup", "2")
        time.sleep(0.5)
        check(editor.result("scene.history")["undo"] == history_before,
              "orbiting the camera adds nothing to the undo history")
        check(editor.result("scene.list")["entities"] == snapshot_before,
              "orbiting the camera does not change any entity")
        editor.screenshot("phase-f-orbited.png")

        # --- Traces must not fill up with the editor's polling. ---
        print("\n[trace] editor polling stays out of traces")
        editor.result("trace.start", filename="editor-interaction.relay-trace.jsonl")
        editor.move(*VIEWPORT_CENTRE)
        xdo("mousedown", "2")
        for step in range(6):
            editor.move(VIEWPORT_CENTRE[0] + step * 8, VIEWPORT_CENTRE[1] + step * 3)
        xdo("mouseup", "2")
        time.sleep(3.0)
        stopped = editor.result("trace.stop")
        check(stopped["events"] <= 2,
              f"editor navigation and three seconds of polling recorded {stopped['events']} trace events")

        print("\nDone.")
    finally:
        editor.close()

    print("\n" + ("ALL INTERACTION CHECKS PASSED" if not FAILURES else f"{len(FAILURES)} FAILED"))
    for failure in FAILURES:
        print("  - " + failure)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
