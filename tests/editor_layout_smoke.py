#!/usr/bin/env python3
"""Real desktop docking, resizing, persistence and viewport alignment checks."""
import re
import tempfile
import time
from pathlib import Path
from editor_interaction_smoke import Editor, tree_row, xdo


def wait_for(check):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if check(): return
        time.sleep(.15)
    raise AssertionError("saved layout did not reflect the completed interaction")


def floating(editor, name):
    section = re.search(r"\[Window\]\["+re.escape(name)+r"\]\n(.*?)(?=\n\[|\Z)", editor.layout_path.read_text(), re.S)
    return section is not None and "DockId=" not in section.group(1)


def main():
    with tempfile.TemporaryDirectory(prefix="relay-layout-smoke-") as directory:
        path = Path(directory) / "layout.ini"
        editor = Editor(layout_path=path)
        try:
            assert editor.calibrate()
            editor.result("runtime.pause")
            probe = editor.result("scene.create", name="Layout probe")["entity"]
            editor.result("scene.set_renderer", entity=probe, mesh="builtin.quad", material="builtin.azure")
            time.sleep(.8)
            editor.click(110, tree_row(0))
            editor.key("f")
            scene = editor.result("scene.list")["entities"]
            history = editor.result("scene.history")
            initial = editor.panel_rect("Hierarchy")
            editor.drag(initial[0]+initial[2]+1, initial[1]+130,
                        initial[0]+initial[2]+101, initial[1]+130)
            wait_for(lambda: editor.panel_rect("Hierarchy")[2] > initial[2]+60)
            assert editor.result("scene.list")["entities"] == scene
            assert editor.result("scene.history") == history
            print("PASS: dock splitter resizes panels without scene or undo mutations", flush=True)

            center = editor.viewport_center()
            original = editor.result("scene.inspect", entity=probe)["transform"]
            editor.drag(*center, center[0]+60, center[1])
            assert editor.result("scene.inspect", entity=probe)["transform"] != original
            editor.key("ctrl+z")
            assert editor.result("scene.inspect", entity=probe)["transform"] == original
            editor.click(center[0]+200, center[1]-100)
            editor.click(*center)
            editor.key("Delete")
            assert not editor.call("scene.inspect", entity=probe)["ok"]
            editor.key("ctrl+z")
            print("PASS: resized viewport gizmo and click-picking remain aligned", flush=True)

            inspector = editor.panel_rect("Inspector")
            xdo("keydown", "Shift_L")
            editor.drag(inspector[0]+70, inspector[1]+13, 760, 250)
            xdo("keyup", "Shift_L")
            wait_for(lambda: floating(editor, "Inspector"))
            inspector = editor.panel_rect("Inspector")
            editor.drag(inspector[0]+inspector[2]-2, inspector[1]+inspector[3]-2,
                        inspector[0]+inspector[2]+88, inspector[1]+inspector[3]+48)
            wait_for(lambda: editor.panel_rect("Inspector")[2] > inspector[2]+50)
            resized = editor.panel_rect("Inspector")
            assert resized[3] > inspector[3]+25, (inspector,resized)
            xdo("keydown", "Shift_L")
            editor.drag(resized[0]+80, resized[1]+13, resized[0]-70, resized[1]+13)
            xdo("keyup", "Shift_L")
            editor.screenshot("phase-f-docking-adjusted.png")
            print("PASS: a panel can undock, resize and move freely", flush=True)
        finally:
            xdo("keyup", "Shift_L")
            editor.close()
        saved = path.read_text()
        editor = Editor(layout_path=path)
        try:
            # Calibration waits for a readable settings file; imported layout is applied at startup.
            assert editor.calibrate()
            assert floating(editor,"Inspector")
            expected = re.search(r"\[Window\]\[Inspector\]\nPos=(.*)\nSize=(.*)", saved).groups()
            rect = editor.panel_rect("Inspector")
            assert rect == tuple(int(v) for line in expected for v in line.split(",")), (rect,expected)
            editor.screenshot("phase-f-docking-restored.png")
            print("PASS: adjusted panel layout survives restart", flush=True)
            editor.click(284,13)
            editor.click(315,40)
            wait_for(lambda: not floating(editor,"Inspector"))
            assert editor.panel_rect("Hierarchy")[2] == initial[2]
            print("PASS: Layout > Reset layout restores docked defaults", flush=True)

            # The scene viewport itself is a movable panel, not a fixed hole in the layout.
            viewport = editor.panel_rect("Viewport")
            xdo("keydown", "Shift_L")
            editor.drag(viewport[0]+35, viewport[1]+13, 520, 220)
            xdo("keyup", "Shift_L")
            wait_for(lambda: floating(editor,"Viewport"))
            viewport = editor.panel_rect("Viewport")
            editor.drag(viewport[0]+viewport[2]-2, viewport[1]+viewport[3]-2,
                        viewport[0]+viewport[2]-102, viewport[1]+viewport[3]+38)
            wait_for(lambda: editor.panel_rect("Viewport")[2] < viewport[2]-60)
            entity = editor.result("scene.create", name="Floating viewport probe")["entity"]
            editor.result("scene.set_renderer", entity=entity, mesh="builtin.quad", material="builtin.azure")
            time.sleep(.8)
            editor.click(110,tree_row(0))
            editor.key("f")
            center = editor.viewport_center()
            editor.click(*center)
            before = editor.result("scene.inspect",entity=entity)["transform"]
            editor.drag(*center,center[0]+60,center[1])
            assert editor.result("scene.inspect",entity=entity)["transform"] != before
            editor.key("ctrl+z")
            assert editor.result("scene.inspect",entity=entity)["transform"] == before
            editor.screenshot("phase-f-docking-floating-viewport.png")
            print("PASS: floating and resized viewport renders and edits at its new position",flush=True)
            xdo("windowsize",editor.window,"1360","800")
            time.sleep(1)
            paused = editor.result("runtime.status")["paused"]
            editor.click(*editor.pause_button)
            assert editor.result("runtime.status")["paused"] != paused
            editor.click(*editor.pause_button)
            xdo("windowsize",editor.window,"1280","720")
            time.sleep(1)
            print("PASS: toolbar input survives native window resize and swapchain recreation",flush=True)


        finally:
            editor.close()


if __name__ == "__main__":
    main()
