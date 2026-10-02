#!/usr/bin/env python3
"""Renders the demo showcase with and without global illumination and ray traced reflections.

The Vulkan renderer runs under SDL's offscreen video driver, so no window appears and the desktop
is untouched. The test skips (and passes) when offscreen Vulkan or the lighting features are
unavailable. It checks that each effect reports itself active, that turning both off still
renders the scene, and that each effect changes the image where it should. A copy of the demo
with a Sky node then checks the gradient sky, fog and a sky material's panorama, and another the
custom surface shaders and post processing. The demo's motion blur must blur a camera that
turns between frames and leave a still one sharp. Finally particles in front of the camera must
glow additively, cover alpha-blended and rise as the capture steps.
"""

import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROJECT = "examples/demo/demo.relayproject"


def read_png(path: pathlib.Path):
    """Decodes an 8-bit RGB or RGBA PNG without third-party modules."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    offset, width, height, channels, idat = 8, 0, 0, 0, b""
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        chunk = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", chunk[:10])
            if depth != 8 or color not in (2, 6):
                raise ValueError("unsupported PNG layout")
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            idat += chunk
        offset += 12 + length
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for row in range(height):
        start = row * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for index in range(stride):
            left = line[index - channels] if index >= channels else 0
            up = previous[index]
            corner = previous[index - channels] if index >= channels else 0
            if kind == 1:
                line[index] = (line[index] + left) & 0xFF
            elif kind == 2:
                line[index] = (line[index] + up) & 0xFF
            elif kind == 3:
                line[index] = (line[index] + (left + up) // 2) & 0xFF
            elif kind == 4:
                estimate = left + up - corner
                pick = min((abs(estimate - left), 0, left), (abs(estimate - up), 1, up),
                           (abs(estimate - corner), 2, corner))[2]
                line[index] = (line[index] + pick) & 0xFF
        rows.append(line)
        previous = line
    return width, height, channels, rows


def mean_difference(first, second) -> float:
    width, height, channels, rows_a = first
    _, _, _, rows_b = second
    total = 0
    for row_a, row_b in zip(rows_a, rows_b):
        for index in range(0, width * channels, channels):
            total += sum(abs(row_a[index + c] - row_b[index + c]) for c in range(3))
    return total / (width * height * 3)


def render(binary: str, output: pathlib.Path, global_illumination: bool, reflections: bool,
           cwd: pathlib.Path = ROOT, project: str = PROJECT, turn: float = 0.0):
    env = {key: value for key, value in os.environ.items()
           if key not in ("WAYLAND_DISPLAY", "DISPLAY")}
    env.update(SDL_VIDEODRIVER="offscreen",
               RELAY_GLOBAL_ILLUMINATION="1" if global_illumination else "0",
               RELAY_REFLECTIONS="1" if reflections else "0")
    result = subprocess.run([binary, "--vulkan-scene-capture", project, str(output), "90", str(turn)],
                            cwd=cwd, env=env, capture_output=True, text=True, timeout=240)
    status = None
    for line in result.stdout.splitlines():
        if line.startswith("{\"global_illumination\""):
            status = json.loads(line)
    return result, status


def write_png(path: pathlib.Path, width: int, height: int, pixel) -> None:
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) + b"\xff" for x in range(width))
                    for y in range(height))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def add_sky(project: pathlib.Path, sky: dict) -> None:
    """Gives a copy of the demo's showcase scene a Sky node, or a new sky for the one it has."""
    path = project / "scenes" / "showcase.relay.json"
    document = json.loads(path.read_text())
    entities = document["scene"]["entities"]
    for entity in entities:
        entity.setdefault("sky", None)
        if entity["sky"] is not None:
            entity["sky"] = sky
            path.write_text(json.dumps(document))
            return
    generations = document["scene"]["allocator"]["slot_generations"]
    node = {key: ([] if key == "disabled_components" else None) for key in entities[0]}
    node.update(entity=f"{len(generations)}:1", name="Sky", scripts=[], sky=sky,
                transform={"position": {"x": 0, "y": 0, "z": 0},
                           "rotation_degrees": {"x": -50, "y": 30, "z": 0},
                           "scale": {"x": 1, "y": 1, "z": 1}})
    generations.append(1)
    entities.append(node)
    document["version"] = max(document["version"], 21)
    path.write_text(json.dumps(document))


def sky_checks(binary: str, folder: pathlib.Path, off_image) -> list:
    failures = []
    project = folder / "skies"
    shutil.copytree(ROOT / "examples" / "demo", project)
    color = lambda r, g, b: {"x": r, "y": g, "z": b}
    # Fog from the camera onwards covers every surface in solid green.
    add_sky(project, {"material": "", "horizon_color": color(0.9, 0.3, 0.1),
                      "zenith_color": color(0.05, 0.15, 0.9), "intensity": 1,
                      "ambient_intensity": 0.25, "fog": True, "fog_start": 0, "fog_end": 0.5,
                      "fog_start_color": color(0, 1, 0), "fog_end_color": color(0, 1, 0)})
    result, status = render(binary, folder / "sky.png", True, True, folder, "skies/demo.relayproject")
    if result.returncode != 0 or status is None:
        return [f"sky render failed: {result.stderr.strip()[-400:]}"]
    width, height, channels, rows = read_png(folder / "sky.png")
    pixel = lambda image, x, y: tuple(image[3][y][x * image[2] + c] for c in range(3))
    image = (width, height, channels, rows)
    top, bottom = pixel(image, width // 2, 2), pixel(image, width // 2, height - 3)
    print(f"sky top {top}, fogged ground {bottom}, plain background {pixel(off_image, width // 2, 2)}")
    if not (top[2] > top[0] + 60 and top[2] > 150):
        failures.append(f"the top of the view should show the blue zenith, not {top}")
    if not (bottom[1] > 150 and bottom[0] < 40 and bottom[2] < 40):
        failures.append(f"fog should cover the ground in its green, not {bottom}")
    # A sky material: magenta above the horizon, dark below.
    write_png(project / "pano.png", 64, 32,
              lambda x, y: (255, 0, 255) if y < 16 else (20, 20, 20))
    (project / "Magenta.relay-material").write_text(json.dumps(
        {"format": "relay.material", "version": 1, "type": "sky", "panorama": "pano.png",
         "tint": color(1, 1, 1), "intensity": 1, "rotation_degrees": 0}))
    add_sky(project, {"material": "Magenta.relay-material", "horizon_color": color(0.9, 0.3, 0.1),
                      "zenith_color": color(0.05, 0.15, 0.9), "intensity": 1,
                      "ambient_intensity": 0.25, "fog": False, "fog_start": 0, "fog_end": 1,
                      "fog_start_color": color(0, 1, 0), "fog_end_color": color(0, 1, 0)})
    result, status = render(binary, folder / "panorama.png", True, True, folder,
                            "skies/demo.relayproject")
    if result.returncode != 0 or status is None:
        return failures + [f"panorama render failed: {result.stderr.strip()[-400:]}"]
    image = read_png(folder / "panorama.png")
    top = pixel(image, width // 2, 2)
    print(f"panorama top {top}")
    if not (top[0] > 150 and top[2] > 150 and top[1] < 60):
        failures.append(f"the sky material's magenta should fill the top of the view, not {top}")
    return failures


def shader_checks(binary: str, folder: pathlib.Path) -> list:
    """A copy of the demo whose ground uses an unshaded custom surface, with its own per-object
    paint color in place of the material's yellow, and whose view passes through a
    post-processing effect that paints its left edge."""
    project = folder / "shaders"
    shutil.copytree(ROOT / "examples" / "demo", project)
    (project / "Paint.relay-shader").write_text(
        "shader_type surface;\nrender_mode unshaded;\n"
        "uniform vec3 paint : source_color = vec3(1.0, 1.0, 0.0);\n"
        "void vertex() { VERTEX.y += 0.0 * TIME; }\n"
        "void fragment() { ALBEDO = paint; }\n")
    (project / "Edge.relay-shader").write_text(
        "shader_type post_process;\n"
        "void fragment() { if (SCREEN_UV.x < 0.05) COLOR = vec3(0.0, 0.0, 4.0); }\n")
    material = lambda kind, shader, parameters: json.dumps(
        {"format": "relay.material", "version": 1, "type": kind, "shader": shader, "parameters": parameters})
    (project / "Paint.relay-material").write_text(material("surface", "Paint.relay-shader", {}))
    (project / "Edge.relay-material").write_text(material("post_process", "Edge.relay-shader", {}))
    path = project / "scenes" / "showcase.relay.json"
    document = json.loads(path.read_text())
    entities = document["scene"]["entities"]
    for entity in entities:
        entity.setdefault("sky", None)
        entity.setdefault("post_process", None)
        if entity["name"] == "Ground":
            entity["mesh_renderer"]["material"] = "Paint.relay-material"
            entity["mesh_renderer"]["parameters"] = {"paint": [0.0, 1.0, 1.0]}
        # Only the new node's effect runs here; the demo's own effects are left out.
        if entity["post_process"] is not None:
            entity["post_process"] = None
    generations = document["scene"]["allocator"]["slot_generations"]
    node = {key: ([] if key == "disabled_components" else None) for key in entities[0]}
    node.update(entity=f"{len(generations)}:1", name="Post", scripts=[],
                post_process={"effects": [{"material": "Edge.relay-material", "enabled": True, "editor": True}]},
                transform={"position": {"x": 0, "y": 0, "z": 0},
                           "rotation_degrees": {"x": 0, "y": 0, "z": 0},
                           "scale": {"x": 1, "y": 1, "z": 1}})
    generations.append(1)
    entities.append(node)
    for entity in entities:
        if entity.get("mesh_renderer"):
            entity["mesh_renderer"].setdefault("parameters", {})
    document["version"] = max(document["version"], 24)
    path.write_text(json.dumps(document))
    result, status = render(binary, folder / "shaded.png", True, True, folder, "shaders/demo.relayproject")
    if result.returncode != 0 or status is None:
        return [f"shader render failed: {result.stderr.strip()[-400:]}"]
    image = read_png(folder / "shaded.png")
    width, height = image[0], image[1]
    pixel = lambda x, y: tuple(image[3][y][x * image[2] + c] for c in range(3))
    ground, edge = pixel(width // 2, height - 3), pixel(4, height // 2)
    print(f"painted ground {ground}, post-processed edge {edge}")
    failures = []
    if not (ground[0] < 90 and ground[1] > 180 and ground[2] > 180):
        failures.append(f"the ground's own paint value should make it cyan, not {ground}")
    if not (edge[2] > 200 and edge[0] < 60 and edge[1] < 60):
        failures.append(f"the post-processing effect should paint the left edge blue, not {edge}")
    if failures:
        return failures

    # Relay's ready-made effects, as the editor copies them into a project.
    for name in ("Bloom", "ColorGrading", "Vignette"):
        shutil.copy(ROOT / "shaders" / "effects" / f"{name}.relay-shader", project / f"{name}.relay-shader")
    (project / "Bloom.relay-material").write_text(material(
        "post_process", "Bloom.relay-shader", {"intensity": [0.5], "threshold": [1.0]}))
    # A small, very bright red spot in the sky for the bloom to spread.
    (project / "Spot.relay-shader").write_text(
        "shader_type post_process;\n"
        "void fragment() { if (distance(SCREEN_UV, vec2(0.5, 0.15)) < 0.015) COLOR = vec3(40.0, 0.0, 0.0); }\n")
    (project / "Spot.relay-material").write_text(material("post_process", "Spot.relay-shader", {}))
    (project / "Grey.relay-material").write_text(material(
        "post_process", "ColorGrading.relay-shader", {"saturation": [0.0]}))
    (project / "Red.relay-material").write_text(material(
        "post_process", "Vignette.relay-shader",
        {"amount": [1.0], "size": [1.2], "softness": [0.2], "color": [1.0, 0.0, 0.0]}))

    def render_effects(effects: list, output: str):
        post = next(entity for entity in entities if entity["name"] == "Post")
        post["post_process"]["effects"] = [{"material": effect, "enabled": True, "editor": True} for effect in effects]
        path.write_text(json.dumps(document))
        result, status = render(binary, folder / output, True, True, folder, "shaders/demo.relayproject")
        if result.returncode != 0 or status is None:
            return None
        return read_png(folder / output)

    # Bloom spreads a bright spot's light into the sky around it.
    plain = render_effects(["Spot.relay-material", "Edge.relay-material"], "spot.png")
    bloomed = render_effects(["Spot.relay-material", "Bloom.relay-material"], "bloomed.png")
    if plain is None or bloomed is None:
        return ["the bloom renders failed"]
    near = (width // 2, int(height * 0.15) + int(height * 0.04))
    before = plain[3][near[1]][near[0] * plain[2]]
    after = bloomed[3][near[1]][near[0] * bloomed[2]]
    print(f"red near the bright spot: {before} without bloom, {after} with it")
    if after < before + 30:
        failures.append(f"bloom should spread the bright spot's light around it ({before} -> {after})")
    graded = render_effects(["Grey.relay-material", "Red.relay-material"], "graded.png")
    if graded is None:
        return ["the color grading render failed"]
    grey = tuple(graded[3][height - 3][width // 2 * graded[2] + c] for c in range(3))
    corner = tuple(graded[3][1][1 * graded[2] + c] for c in range(3))
    print(f"graded ground {grey}, vignette corner {corner}")
    if max(grey) - min(grey) > 6:
        failures.append(f"color grading with no saturation should turn the cyan ground grey, not {grey}")
    if not (corner[0] > 200 and corner[1] < 40 and corner[2] < 40):
        failures.append(f"a red vignette should color the corners red, not {corner}")
    return failures


def material_preview_checks(binary: str, folder: pathlib.Path) -> list:
    """The Inspector's material preview: the shader checks' unshaded yellow paint on a sphere,
    with the checkerboard around it."""
    env = {key: value for key, value in os.environ.items() if key not in ("WAYLAND_DISPLAY", "DISPLAY")}
    env.update(SDL_VIDEODRIVER="offscreen")
    output = folder / "preview.png"
    result = subprocess.run([binary, "--vulkan-material-preview", "shaders/demo.relayproject",
                             "Paint.relay-material", str(output)],
                            cwd=folder, env=env, capture_output=True, text=True, timeout=240)
    if result.returncode != 0:
        return [f"material preview failed: {(result.stderr or result.stdout).strip()[-400:]}"]
    image = read_png(output)
    width, height = image[0], image[1]
    pixel = lambda x, y: tuple(image[3][y][x * image[2] + c] for c in range(3))
    centre, corner = pixel(width // 2, height // 2), pixel(2, 2)
    print(f"preview {width}x{height}: sphere {centre}, background {corner}")
    failures = []
    if not (centre[0] > 180 and centre[1] > 180 and centre[2] < 90):
        failures.append(f"the preview sphere should show the material's yellow, not {centre}")
    if not (max(corner) < 110 and max(corner) - min(corner) < 10):
        failures.append(f"the preview's background should be a grey checkerboard, not {corner}")
    return failures


def sharp_edges(image) -> int:
    """Pixels whose color jumps sharply from their left neighbour: blur makes them rare."""
    width, height, channels, rows = image
    count = 0
    for row in rows:
        for index in range(channels, width * channels, channels):
            if sum(abs(row[index + c] - row[index + c - channels]) for c in range(3)) > 120:
                count += 1
    return count


def hide_interface(project: pathlib.Path) -> None:
    """Hides every canvas in a copy of the demo's showcase, so only the 3D view is measured."""
    path = project / "scenes" / "showcase.relay.json"
    document = json.loads(path.read_text())
    for entity in document["scene"]["entities"]:
        if (entity.get("ui") or {}).get("canvas"):
            entity["ui"]["canvas"]["visible"] = False
    path.write_text(json.dumps(document))


def motion_blur_checks(binary: str, folder: pathlib.Path) -> list:
    """The demo's motion blur: a still camera stays sharp, a camera turning between frames blurs."""
    # The game interface draws over the view and never blurs, so the copy leaves it out.
    project = folder / "blur-demo"
    shutil.copytree(ROOT / "examples" / "demo", project, ignore=shutil.ignore_patterns(".relay-cache"))
    hide_interface(project)
    still_result, _ = render(binary, folder / "still.png", True, True, cwd=folder,
                             project="blur-demo/demo.relayproject")
    turning_result, _ = render(binary, folder / "turning.png", True, True, cwd=folder,
                               project="blur-demo/demo.relayproject", turn=3.0)
    if still_result.returncode != 0 or turning_result.returncode != 0:
        return [f"motion blur renders failed: {(still_result.stderr or turning_result.stderr).strip()[-400:]}"]
    still, turning = sharp_edges(read_png(folder / "still.png")), sharp_edges(read_png(folder / "turning.png"))
    print(f"sharp edges still {still}, turning {turning}")
    return [] if turning < still * 0.5 else [f"a turning camera should blur the view ({still} -> {turning} sharp edges)"]


def particle_checks(binary: str, folder: pathlib.Path) -> list:
    """Particles in front of the demo's camera: a glowing additive sprite in the middle of the view,
    an opaque alpha-blended one to its left, and a stream of lit smoke that must have been simulated
    through the capture's frames. Each is checked by the pixels it covers."""
    project = folder / "particle-demo"
    shutil.copytree(ROOT / "examples" / "demo", project, ignore=shutil.ignore_patterns(".relay-cache"))
    hide_interface(project)
    path = project / "scenes" / "showcase.relay.json"
    document = json.loads(path.read_text())
    entities = document["scene"]["entities"]
    for entity in entities:
        entity.setdefault("particle_emitter", None)
        # Only these particles, and no post effects spreading them.
        entity["particle_emitter"] = None
        if entity.get("post_process") is not None:
            entity["post_process"] = None
    camera = next(entity for entity in entities if entity.get("camera") and entity["camera"]["active"])
    generations = document["scene"]["allocator"]["slot_generations"]

    def add(name: str, position: list, emitter: dict) -> None:
        node = {key: ([] if key == "disabled_components" else None) for key in entities[0]}
        node.update(entity=f"{len(generations)}:1", name=name, scripts=[], parent=camera["entity"],
                    particle_emitter=emitter,
                    transform={"position": dict(zip("xyz", position)), "rotation_degrees": {"x": 0, "y": 0, "z": 0},
                               "scale": {"x": 1, "y": 1, "z": 1}})
        generations.append(1)
        entities.append(node)

    one = {"rate": 0, "bursts": [{"time": 0, "count": 1}], "shape": "point", "speed": 0, "lifetime": 100,
           "soft_distance": 0}
    add("Glow", [0, 0, -1.2], dict(one, size=0.4, color=[1, 0.1, 0.05, 1], blend="additive", emission=6,
                                 builtin_texture="square"))
    add("Blue", [-0.45, 0, -1.2], dict(one, size=0.2, color=[0.05, 0.2, 1, 1], builtin_texture="square"))
    # Rises from below the view to its lower right: nothing shows there unless the steps ran.
    add("Smoke", [0.6, -1.0, -1.2], {"rate": 40, "shape": "point", "speed": 0.5, "direction_randomness": 0,
                                   "spherize": 0, "angle": 0, "lifetime": 3, "size": 0.12, "lit": True,
                                   "color": [0.9, 0.9, 0.9, 1], "builtin_texture": "square", "soft_distance": 0})
    document["version"] = max(document["version"], 26)
    path.write_text(json.dumps(document))
    result, status = render(binary, folder / "particles.png", True, True, folder, "particle-demo/demo.relayproject")
    if result.returncode != 0 or status is None:
        return [f"particle render failed: {result.stderr.strip()[-400:]}"]
    # The same view without the emitters, for what lies behind them.
    for entity in entities:
        entity["particle_emitter"] = None
    path.write_text(json.dumps(document))
    result, status = render(binary, folder / "no-particles.png", True, True, folder, "particle-demo/demo.relayproject")
    if result.returncode != 0 or status is None:
        return [f"render without particles failed: {result.stderr.strip()[-400:]}"]
    image, plain = read_png(folder / "particles.png"), read_png(folder / "no-particles.png")
    width, height = image[0], image[1]
    pixel = lambda picture, x, y: tuple(picture[3][y][x * picture[2] + c] for c in range(3))
    # 1.2 m in front of a camera with a 75 degree view, the half width is about 1.2 m.
    half_width = 1.2 * 0.7673 * width / height
    centre = (width // 2, height // 2)
    glow, behind_glow = pixel(image, *centre), pixel(plain, *centre)
    blue = pixel(image, int(width * (0.5 - 0.45 / half_width / 2.0)), height // 2)
    smoke_x = int(width * (0.5 + 0.6 / half_width / 2.0))
    smoke_change = max(sum(abs(a - b) for a, b in zip(pixel(image, smoke_x, y), pixel(plain, smoke_x, y)))
                       for y in range(height // 2, height - 10, 4))
    print(f"additive glow {glow} over {behind_glow}, alpha sprite {blue}, lit smoke changes the view by {smoke_change}")
    failures = []
    if not (glow[0] >= min(behind_glow[0] + 30, 250) and glow[0] - glow[1] > behind_glow[0] - behind_glow[1] + 30):
        failures.append(f"the additive particle should add red light in the middle of the view: {behind_glow} -> {glow}")
    if not (blue[2] > 150 and blue[0] < 90 and blue[2] > blue[1]):
        failures.append(f"the alpha-blended particle should cover its spot in blue, not {blue}")
    if smoke_change < 60:
        failures.append(f"the lit smoke stream should rise into view as the capture steps (change {smoke_change})")
    return failures


def main() -> int:
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="relay-lighting-") as directory:
        folder = pathlib.Path(directory)
        off_result, off_status = render(binary, folder / "off.png", False, False)
        if off_result.returncode != 0 or off_status is None:
            print("SKIP: offscreen Vulkan rendering is unavailable here:",
                  (off_result.stderr or off_result.stdout).strip()[-400:])
            return 0
        if not off_status["global_illumination"]["device_supported"] or \
                not off_status["reflections"]["device_supported"]:
            print("SKIP: this GPU lacks the features global illumination or ray queries need")
            return 0
        gi_result, gi_status = render(binary, folder / "gi.png", True, False)
        reflection_result, reflection_status = render(binary, folder / "reflections.png", False, True)
        both_result, both_status = render(binary, folder / "both.png", True, True)
        failures = []
        for name, result, status in (("GI", gi_result, gi_status),
                                     ("reflections", reflection_result, reflection_status),
                                     ("both", both_result, both_status)):
            if result.returncode != 0 or status is None:
                failures.append(f"{name} render failed: {result.stderr.strip()[-400:]}")
        if not failures:
            if not gi_status["global_illumination"]["active"] or gi_status["global_illumination"]["error"]:
                failures.append(f"global illumination did not run: {gi_status}")
            if not reflection_status["reflections"]["active"] or reflection_status["reflections"]["error"]:
                failures.append(f"reflections did not run: {reflection_status}")
            if not (both_status["global_illumination"]["active"] and both_status["reflections"]["active"]):
                failures.append(f"the effects did not run together: {both_status}")
            images = {name: read_png(folder / f"{name}.png")
                      for name in ("off", "gi", "reflections", "both")}
            # Each effect changes the image, but the scene stays the scene: a small mean change.
            for name in ("gi", "reflections", "both"):
                change = mean_difference(images["off"], images[name])
                print(f"mean change with {name}: {change:.2f}")
                if not 0.05 < change < 25.0:
                    failures.append(f"{name} changed the image by {change:.2f} levels on average")
        if not failures:
            failures += sky_checks(binary, folder, images["off"])
        if not failures:
            failures += shader_checks(binary, folder)
        if not failures:
            failures += material_preview_checks(binary, folder)
        if not failures:
            failures += motion_blur_checks(binary, folder)
        if not failures:
            failures += particle_checks(binary, folder)
        for failure in failures:
            print("FAIL:", failure)
        if failures:
            return 1
    print("Lighting render tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
