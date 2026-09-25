#!/usr/bin/env python3
"""Renders the demo showcase with and without global illumination and ray traced reflections.

The Vulkan renderer runs under SDL's offscreen video driver, so no window appears and the desktop
is untouched. The test skips (and passes) when offscreen Vulkan or the lighting features are
unavailable. It checks that each effect reports itself active, that turning both off still
renders the scene, and that each effect changes the image where it should.
"""

import json
import os
import pathlib
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


def render(binary: str, output: pathlib.Path, global_illumination: bool, reflections: bool):
    env = {key: value for key, value in os.environ.items()
           if key not in ("WAYLAND_DISPLAY", "DISPLAY")}
    env.update(SDL_VIDEODRIVER="offscreen",
               RELAY_GLOBAL_ILLUMINATION="1" if global_illumination else "0",
               RELAY_REFLECTIONS="1" if reflections else "0")
    result = subprocess.run([binary, "--vulkan-scene-capture", PROJECT, str(output), "90"],
                            cwd=ROOT, env=env, capture_output=True, text=True, timeout=240)
    status = None
    for line in result.stdout.splitlines():
        if line.startswith("{\"global_illumination\""):
            status = json.loads(line)
    return result, status


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
        for failure in failures:
            print("FAIL:", failure)
        if failures:
            return 1
    print("Lighting render tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
