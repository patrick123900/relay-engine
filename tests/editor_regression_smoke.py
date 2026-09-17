#!/usr/bin/env python3
"""Real-input regressions for inspector drafts, protocol decoding and viewport alignment.

Run on the Linux desktop with xdotool. No scene/manifest is saved; screenshots are optional and
written to a temporary directory. Engine state and selection outcomes are asserted via stdio.
"""
import time
from editor_interaction_smoke import Editor, tree_row


def main():
    editor = Editor()
    try:
        assert editor.calibrate(), "toolbar coordinate calibration failed"
        editor.result("runtime.pause")
        editor.drag(850, 486, 850, 640)
        time.sleep(6)
        center = editor.viewport_center()
        probe = editor.result("scene.create", name="Inspector regression")["entity"]
        editor.result("scene.set_transform", entity=probe, px=2, py=3, pz=4,
                      rx=10, ry=20, rz=30, sx=2, sy=3, sz=4)
        editor.result("scene.set_light", entity=probe, type="spot", intensity=2,
                      red=.2, green=.3, blue=.4)
        time.sleep(.8)
        editor.click(110, tree_row(0))
        before = editor.result("scene.inspect", entity=probe)["transform"]
        editor.drag(1120, 301, 1160, 301)
        after = editor.result("scene.inspect", entity=probe)["transform"]
        if before["position"]["x"] == after["position"]["x"]:
            editor.screenshot("/tmp/relay-regression-failure.png")
        assert before["position"]["x"] != after["position"]["x"], (before, after)
        assert after["position"]["y"] == 3 and after["position"]["z"] == 4, after
        assert after["rotation_degrees"] == before["rotation_degrees"] and after["scale"] == before["scale"], after
        print("PASS: position drag preserves untouched position, rotation and scale", flush=True)

        editor.drag(1100, 564, 1160, 564)
        light = editor.result("scene.inspect", entity=probe)["light"]
        assert light["intensity"] > 2 and light["type"] == 2, light
        assert light["color"] == {"x": .2, "y": .3, "z": .4}, light
        print("PASS: spot-light drag persists across release and preserves type/color", flush=True)

        editor.result("scene.set_transform", entity=probe, py=99, rx=45, sx=3)
        time.sleep(.8)
        editor.drag(1120, 301, 1160, 301)
        after_agent = editor.result("scene.inspect", entity=probe)["transform"]
        assert after_agent["position"]["y"] == 99 and after_agent["rotation_degrees"]["x"] == 45 and after_agent["scale"]["x"] == 3, after_agent
        editor.key("ctrl+z")
        undo = editor.result("scene.inspect", entity=probe)["transform"]
        assert undo["position"]["x"] != after_agent["position"]["x"], (undo, after_agent)
        assert undo["position"]["y"] == 99 and undo["rotation_degrees"]["x"] == 45, undo
        print("PASS: inspector follows agent state; undo restores only the human edit", flush=True)

        editor.result("scene.set_transform", entity=probe, px=0, py=0, pz=0,
                      rx=0, ry=0, rz=0, sx=1, sy=1, sz=1)
        editor.result("scene.set_renderer", entity=probe, mesh="builtin.quad", material="builtin.azure")
        time.sleep(.8)
        editor.key("f")
        time.sleep(.5)
        # Clear the current selection by clicking empty viewport space, then click the actual
        # projected center. The old full-window renderer put the object 92px below this point.
        editor.click(900, 180)
        editor.click(*center)
        editor.key("Delete")
        response = editor.call("scene.inspect", entity=probe)
        assert not response["ok"], response
        print("PASS: viewport click selects the rendered object at its projected center", flush=True)
        editor.key("ctrl+z")

        editor.click(110, tree_row(0))
        editor.key("w")
        before = editor.result("scene.inspect", entity=probe)["transform"]
        depth = len(editor.result("scene.history")["undo"])
        editor.drag(*center, center[0]+80, center[1])
        after = editor.result("scene.inspect", entity=probe)["transform"]
        assert after != before and len(editor.result("scene.history")["undo"]) == depth + 1, (before, after)
        editor.key("ctrl+z")
        assert editor.result("scene.inspect", entity=probe)["transform"] == before
        print("PASS: aligned gizmo gesture is one complete undo step", flush=True)

        editor.result("scene.set_camera", entity=probe, active=False, near_plane=.2, far_plane=100)
        time.sleep(.8)
        editor.drag(1100, 455, 1150, 455)
        camera = editor.result("scene.inspect", entity=probe)["camera"]
        assert camera["field_of_view_y_degrees"] > 60 and camera["near_plane"] == .2 and camera["far_plane"] == 100 and not camera["active"], camera
        print("PASS: camera drag persists without overwriting other camera parameters", flush=True)
        editor.result("scene.set_camera", entity=probe, enabled=False)
        editor.result("scene.set_light", entity=probe, enabled=False)
        editor.result("scene.destroy", entity=probe)
        editor.result("assets.import_model", filename="relay-dynamic-golden.gltf")
        time.sleep(1)
        entities = editor.result("scene.list")["entities"]
        mesh = next(item for item in entities if item["mesh_renderer"])
        assert mesh["mesh_renderer"]["morph_weights"] == [], mesh
        editor.click(110, tree_row(entities.index(mesh)))
        editor.click(1120, 266)  # collapse Transform to expose the morph section
        editor.click(1120, 465)  # Morph targets, after Camera/Mesh sections
        editor.drag(1100, 524, 1150, 524)
        renderer = editor.result("scene.inspect", entity=mesh["entity"])["mesh_renderer"]
        assert renderer["morph_weights"] and renderer["morph_weights"][0] != 0, renderer
        print("PASS: imported morph target can receive its first override entirely through the inspector", flush=True)
        editor.result("render.capture", path="captures/phase-f-audit-vulkan.png", source="vulkan")
        editor.screenshot("phase-f-audit-editor.png")
    finally:
        editor.close()
    print("Editor regressions passed")


if __name__ == "__main__":
    main()
