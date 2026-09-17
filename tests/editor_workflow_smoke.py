#!/usr/bin/env python3
"""Real desktop regression for the everyday editing and save workflow.

Drives duplication, the unsaved-work guard, save/open and the animator controls with actual
keyboard and mouse input, then asserts the result over the stdio protocol channel rather than by
reading screenshots. The window title is read back from the window manager, because that is where
a person actually looks to see which scene they are editing and whether it still needs saving.

Requires a desktop session and xdotool, and uses an isolated saved layout.
Run from the repository root: python3 tests/editor_workflow_smoke.py
"""
import sys
import time
from pathlib import Path

from editor_interaction_smoke import ROOT, FAILURES, Editor, check, xdo

SCENE_FILE = "workflow-smoke.relay.json"


def title(editor):
    return xdo("getwindowname", editor.window)


def shortcut(editor, *keys):
    """Send a menu shortcut to the focused editor.

    Delivered through XTEST rather than the harness's usual per-window send: measured against a
    repeated Ctrl+O, the per-window form reached the editor about half the time and XTEST most of
    the time. Neither is perfect on this desktop, so interactions with a checkable outcome are
    wrapped in `until` rather than assumed to have landed.
    """
    xdo("windowactivate", "--sync", editor.window)
    time.sleep(0.2)
    for key in keys:
        xdo("key", "--clearmodifiers", key)
        time.sleep(0.3)


def until(action, condition, attempts=3, settle=1.5):
    """Repeat an interaction until its effect shows up in engine state.

    This desktop occasionally drops a synthetic key, and a dropped one is indistinguishable from
    an editor that ignored it. Retrying an interaction that has a checkable outcome keeps the
    suite reporting on the editor rather than on the compositor, and a genuine regression still
    fails because the outcome never arrives.
    """
    for attempt in range(attempts):
        action()
        time.sleep(settle)
        if condition():
            return True
        if attempt + 1 < attempts:
            print(f"   no effect on attempt {attempt + 1}; retrying")
    return False


def entities(editor):
    return {item["entity"]: item for item in editor.result("scene.list")["entities"]}


def undo_depth(editor):
    return editor.result("scene.history")["state"]["undo_depth"]


def animator_of(editor, entity):
    return editor.result("scene.inspect", entity=entity)["animator"]


def wait_for_layout(editor, timeout=20):
    """Panel geometry is read from the saved layout, which ImGui only writes once it settles."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if editor.layout_path.exists():
            try:
                editor.panel_rect("Viewport")
                return
            except RuntimeError:
                pass
        time.sleep(0.25)
    raise RuntimeError("editor never wrote its panel layout")


def file_dialog(editor, keys, filename):
    """Run a project filename dialog from the keyboard.

    A dialog that reported an error stays open by design, and so does one left behind by a
    dropped keystroke, so each attempt starts by dismissing whatever is on screen. The filename
    field takes keyboard focus as it appears, which is what makes this pure keyboard input.
    """
    shortcut(editor, "Escape")
    shortcut(editor, *keys)
    time.sleep(1.2)
    # Clear the prefilled name explicitly rather than trusting the field's initial select-all to
    # survive synthetic input. One run appended the new name to the old one and asked the editor
    # to open "<name><name>", which failed and left the dialog sitting open with its error.
    xdo("key", "--clearmodifiers", "End")
    time.sleep(0.15)
    xdo("key", "--repeat", "140", "--delay", "6", "--clearmodifiers", "BackSpace")
    time.sleep(0.3)
    editor.type_text(filename)
    editor.key("Return")
    time.sleep(1.5)


def find_animator_controls(editor, entity, rect):
    """Locate the Play and Restart buttons by their effect rather than by fixed coordinates.

    The animator row sits below however many component headers the selection happens to have, so
    scanning and checking engine state keeps this working when the inspector is restyled. Every
    miss is clicked a second time, which leaves a collapsing header exactly as it was found and
    stops the scan from shifting the layout underneath itself.
    """
    left, top, width, height = rect
    column = left + 45
    for y in range(top + 60, min(top + height - 20, top + 620), 9):
        before = animator_of(editor, entity)["playing"]
        editor.click(column, y)
        if animator_of(editor, entity)["playing"] == before:
            editor.click(column, y)
            continue
        editor.click(column, y)  # Leave playback as it was found.
        play = (column, y)
        # Restart shares the row, to the right of the loop toggle.
        editor.result("scene.set_animation", entity=entity, time_seconds=0.25)
        for probe in range(column + 20, left + width - 10, 14):
            editor.click(probe, y)
            if animator_of(editor, entity)["time_seconds"] == 0.0:
                return play, (probe, y)
        return play, None
    return None, None


def main():
    saved = Path(ROOT) / "scenes" / SCENE_FILE
    saved.unlink(missing_ok=True)
    editor = Editor()
    try:
        editor.result("runtime.pause")

        print("\n[unsaved marker] editing an untitled scene")
        probe = editor.result("scene.create", name="Probe")["entity"]
        editor.result("scene.set_renderer", entity=probe, enabled=True,
                      mesh="builtin.quad", material="builtin.azure")
        editor.result("scene.create", name="ProbeChild", parent=probe)
        time.sleep(2)
        check("Untitled scene" in title(editor) and title(editor).endswith("Relay Editor"),
              "an unsaved scene is titled as untitled in the window title")
        check("*" in title(editor), "the window title marks unsaved work after an edit")

        print("\n[duplicate] Ctrl+D on a viewport selection")
        wait_for_layout(editor)
        editor.click(*editor.viewport_center())
        time.sleep(1)
        before = entities(editor)
        depth = undo_depth(editor)
        until(lambda: shortcut(editor, "ctrl+d"),
              lambda: len(entities(editor)) == len(before) + 2)
        after = entities(editor)
        added = [handle for handle in after if handle not in before]
        check(len(added) == 2, "duplicating a parent copies its child too")
        copies = [after[handle] for handle in added if after[handle]["parent"] is None]
        check(len(copies) == 1 and copies[0]["name"] == "Probe Copy",
              "the duplicated root is named apart from the original")
        check(undo_depth(editor) == depth + 1, "a duplication is one undo entry")

        # Deleting proves which entity the editor selected: the copy, not what it was made from.
        copy_handle = copies[0]["entity"] if copies else None
        shortcut(editor, "Delete")
        time.sleep(1)
        check(copy_handle is not None and copy_handle not in entities(editor)
              and probe in entities(editor),
              "duplication selects the copy, so the next edit lands on it")
        shortcut(editor, "ctrl+z")
        time.sleep(1)
        check(copy_handle in entities(editor), "undo restores the deleted copy")
        shortcut(editor, "ctrl+z")
        time.sleep(1)
        check(copy_handle not in entities(editor) and len(entities(editor)) == len(before),
              "a second undo rewinds the whole duplication")

        print("\n[unsaved guard] Ctrl+N with unsaved work")
        populated = len(entities(editor))
        shortcut(editor, "ctrl+n")
        time.sleep(1.5)
        check(len(entities(editor)) == populated,
              "starting a new scene with unsaved work prompts instead of discarding it")
        shortcut(editor, "Escape")
        time.sleep(1.5)
        check(len(entities(editor)) == populated,
              "cancelling the prompt leaves the scene untouched")
        # Shortcuts are suppressed while any popup is up, so a working Ctrl+D is how this suite
        # tells a dismissed prompt from one that is still sitting there invisibly blocking input.
        editor.click(*editor.viewport_center())
        time.sleep(1)
        depth = undo_depth(editor)
        check(until(lambda: shortcut(editor, "ctrl+d"),
                    lambda: undo_depth(editor) == depth + 1),
              "cancelling the prompt hands keyboard shortcuts back to the editor")
        shortcut(editor, "ctrl+z")
        time.sleep(1)

        print("\n[save] Ctrl+Shift+S")
        check(until(lambda: file_dialog(editor, ["ctrl+shift+s"], SCENE_FILE),
                    lambda: saved.exists()),
              "Save as writes the scene into scenes/")
        check(SCENE_FILE in title(editor), "the window title names the saved file")
        check("*" not in title(editor), "saving clears the unsaved marker")
        # What the file actually holds, rather than what the scene held earlier: a retry above may
        # have saved a slightly different scene, and reopening has to match the file.
        saved_count = len(entities(editor))

        print("\n[new scene] Ctrl+N once the scene is saved")
        check(until(lambda: shortcut(editor, "ctrl+n"), lambda: entities(editor) == {}, settle=2),
              "a saved scene is cleared without a prompt")
        check("Untitled scene" in title(editor),
              "a new scene is untitled again, so the next save asks for a name")

        print("\n[reopen] Ctrl+O")
        check(until(lambda: file_dialog(editor, ["ctrl+o"], SCENE_FILE),
                    lambda: len(entities(editor)) == saved_count),
              "reopening the saved scene restores every entity")
        check(SCENE_FILE in title(editor) and "*" not in title(editor),
              "a freshly opened scene is not reported as modified")

        print("\n[animation] clip metadata and the inspector controls")
        editor.result("scene.clear")
        imported = editor.result("assets.import_model",
                                 filename="relay-dynamic-golden.gltf", instantiate=True)
        rig = imported["roots"][0]
        model = animator_of(editor, rig)["model"]
        clips = next(entry["clips"] for entry in editor.result("render.assets")["models"]
                     if entry["name"] == model)
        check(bool(clips) and all(clip["duration_seconds"] > 0 for clip in clips),
              "the animator panel has named clips with durations to bound its time slider")

        # The animator lives on the imported root, but the model's geometry sits on a child, so a
        # viewport click would select the child and inspect a component that is not there. Making
        # the root the only drawable long enough to click it selects the entity under test; the
        # placeholder renderer is then removed, leaving the inspector showing the real component.
        for item in editor.result("scene.list")["entities"]:
            if item["mesh_renderer"]:
                editor.result("scene.destroy", entity=item["entity"])
        editor.result("scene.set_renderer", entity=rig, enabled=True,
                      mesh="builtin.quad", material="builtin.azure")
        time.sleep(2)
        editor.click(*editor.viewport_center())
        time.sleep(1)
        editor.result("scene.set_renderer", entity=rig, enabled=False)
        editor.result("scene.set_animation", entity=rig, playing=False, time_seconds=0.1)
        time.sleep(1.5)
        inspector = editor.panel_rect("Inspector")
        play, restart = find_animator_controls(editor, rig, inspector)
        check(play is not None, "the inspector play control starts and stops the selected animator")
        check(restart is not None, "the inspector restart control seeks the clip back to zero")
        if play is not None:
            left, top, width, height = inspector
            found_slider = False
            # Probe the rows below Play, checking while the mouse is still held down.
            # This fails for a widget that only sends its seek on release.
            for y in range(play[1] + 30, min(play[1] + 130, top + height - 20), 5):
                editor.result("scene.set_animation", entity=rig, playing=False, time_seconds=0.1)
                time.sleep(0.6)
                depth = undo_depth(editor)
                editor.move(left + width // 2, y)
                xdo("mousedown", "1")
                try:
                    time.sleep(0.2)
                    editor.move(left + width // 2 + 25, y)
                    time.sleep(0.3)
                    during = animator_of(editor, rig)["time_seconds"]
                    found_slider = during != 0.1
                    if found_slider:
                        check(undo_depth(editor) == depth + 1,
                              "scrubbing updates the pose before release in one undo entry")
                        editor.move(left + width // 2 - 25, y)
                        time.sleep(0.3)
                        check(animator_of(editor, rig)["time_seconds"] != during
                              and undo_depth(editor) == depth + 1,
                              "moving the held slider updates the pose without adding undo entries")
                finally:
                    xdo("mouseup", "1")
                if found_slider:
                    break
            check(found_slider, "the time slider applies seeks while the mouse is held down")
        if restart is not None:
            editor.result("scene.set_animation", entity=rig, time_seconds=0.3)
            depth = undo_depth(editor)
            editor.click(*restart)
            check(animator_of(editor, rig)["time_seconds"] == 0.0
                  and undo_depth(editor) == depth + 1,
                  "restarting a clip is one seek and one undo entry")
    finally:
        editor.close()
        saved.unlink(missing_ok=True)

    print()
    if FAILURES:
        print(f"{len(FAILURES)} workflow checks failed:")
        for failure in FAILURES:
            print("  - " + failure)
        return 1
    print("Editor workflow smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
