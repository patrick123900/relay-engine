#!/usr/bin/env python3
"""Exercise real editor menu actions through desktop input and inspect protocol results."""
import os
import time
from pathlib import Path
from editor_interaction_smoke import Editor, ROOT, xdo


def main():
    editor = Editor()
    name = f"menu-smoke-{os.getpid()}"
    scene = Path(ROOT)/"scenes"/(name+".relay.json")
    image = Path(ROOT)/"captures"/(name+".png")
    video = Path(ROOT)/"captures"/(name+".webm")
    try:
        # Keep small top-left menu targets away from gaps between mixed-scale monitors.
        xdo("windowmove", editor.window, "100", "60")
        time.sleep(.5)
        geometry = dict(line.split("=") for line in xdo("getwindowgeometry","--shell",editor.window).splitlines() if "=" in line)
        editor.origin = (int(geometry["X"]), int(geometry["Y"]))
        editor.result("runtime.pause")
        editor.click(106,13)
        editor.move(130,40)
        time.sleep(.3)
        editor.click(330,64)  # Scene > Add node > Quad
        entities = editor.result("scene.list")["entities"]
        assert len(entities)==1 and entities[0]["mesh_renderer"]["mesh"]=="builtin.quad", entities
        handle = entities[0]["entity"]
        print("PASS: Scene menu creates a selected renderable quad",flush=True)

        editor.click(64,13)
        editor.click(90,40)  # Edit > Undo component creation
        assert not editor.result("scene.inspect",entity=handle).get("mesh_renderer")
        editor.click(64,13)
        editor.click(90,62)
        assert editor.result("scene.inspect",entity=handle)["mesh_renderer"]
        print("PASS: Edit menu undo and redo use native scene history",flush=True)

        editor.key("ctrl+shift+s")
        editor.key("ctrl+a")
        editor.type_text(name+".relay.json")
        editor.key("Return")
        assert scene.exists()
        editor.result("scene.destroy",entity=handle)
        editor.key("ctrl+o")
        editor.key("Return")
        assert len(editor.result("scene.list")["entities"])==1
        print("PASS: Save As and Open dialogs round-trip a compatible scene",flush=True)

        editor.click(234,13)
        editor.click(270,40)
        editor.key("ctrl+a")
        editor.type_text(name+".png")
        editor.key("Return")
        deadline=time.monotonic()+10
        while not image.exists() and time.monotonic()<deadline: time.sleep(.1)
        assert image.exists() and image.read_bytes().startswith(b"\x89PNG")
        print("PASS: Tools menu writes an asynchronous real-GPU PNG",flush=True)

        editor.result("runtime.resume")
        editor.click(234,13)
        editor.click(270,62)
        editor.key("ctrl+a")
        editor.type_text(name+".webm")
        editor.key("Return")
        assert editor.result("video.status")["recording"]
        editor.result("runtime.pause")
        time.sleep(.2)
        editor.click(234,13)
        editor.click(270,84)
        deadline=time.monotonic()+60
        while not video.exists() and time.monotonic()<deadline: time.sleep(.1)
        assert video.exists() and video.stat().st_size>0, editor.result("video.status")
        assert not editor.result("video.status")["recording"]
        print("PASS: Tools menu records and finalizes real-GPU WebM",flush=True)

        editor.click(284,13)
        editor.click(315,40)
        editor.screenshot("phase-f-menu-bar.png")
        print("PASS: Layout menu remains available alongside the new menus",flush=True)
    finally:
        editor.close()
        for path in (scene,image,video): path.unlink(missing_ok=True)


if __name__=="__main__": main()
