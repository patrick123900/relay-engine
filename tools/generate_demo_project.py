#!/usr/bin/env python3
"""Regenerate the dev-build demo project in examples/demo.

Writes examples/demo/models/primitives.glb (unit primitives and a small PBR material library), then
runs the native builder, which imports it through the control protocol and authors the showcase
scene. Run from the repository root after building the dev preset:

    python3 tools/generate_demo_project.py
"""
import json
import math
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJECT = ROOT / "examples" / "demo"
BUILDER = ROOT / "build" / "dev" / "relay_build_demo_project"


def cube():
    positions, normals, uvs, indices = [], [], [], []
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    for normal, u, v in faces:
        base = len(positions)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            positions.append(tuple(0.5 * (n + su * a + sv * b) for n, a, b in zip(normal, u, v)))
            normals.append(normal)
            uvs.append(((su + 1) / 2, (1 - sv) / 2))
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


def lathe(profile, segments=40):
    """Revolve (radius, height, normal_radius, normal_height) rings around Y."""
    positions, normals, uvs, indices = [], [], [], []
    for ring, (radius, height, nr, ny) in enumerate(profile):
        for step in range(segments + 1):
            angle = 2 * math.pi * step / segments
            c, s = math.cos(angle), math.sin(angle)
            positions.append((radius * c, height, -radius * s))
            length = math.hypot(nr, ny) or 1.0
            normals.append((nr * c / length, ny / length, -nr * s / length))
            uvs.append((step / segments, ring / (len(profile) - 1)))
    for ring in range(len(profile) - 1):
        for step in range(segments):
            a = ring * (segments + 1) + step
            b = a + segments + 1
            indices += [a, a + 1, b, a + 1, b + 1, b]
    return positions, normals, uvs, indices


def sphere(radius=0.5, rings=20):
    profile = []
    for ring in range(rings + 1):
        phi = math.pi * ring / rings
        profile.append((radius * math.sin(phi), -radius * math.cos(phi),
                        math.sin(phi), -math.cos(phi)))
    return lathe(profile)


def capsule(radius=0.25, half_height=0.5, cap_rings=10):
    profile = []
    for ring in range(cap_rings + 1):
        phi = 0.5 * math.pi * ring / cap_rings
        profile.append((radius * math.sin(phi), -half_height - radius * math.cos(phi),
                        math.sin(phi), -math.cos(phi)))
    for ring in range(cap_rings + 1):
        phi = 0.5 * math.pi + 0.5 * math.pi * ring / cap_rings
        profile.append((radius * math.sin(phi), half_height - radius * math.cos(phi),
                        math.sin(phi), -math.cos(phi)))
    return lathe(profile)


def cylinder(radius=0.5, half_height=0.5):
    side = lathe([(radius, -half_height, 1, 0), (radius, half_height, 1, 0)])
    caps = [lathe([(0, y, 0, n), (radius, y, 0, n)]) for y, n in ((half_height, 1), (-half_height, -1))]
    # The top cap's profile runs outward, opposite to a sphere's crown, so it winds the other way.
    caps[0][3][:] = [index for tri in zip(*[iter(caps[0][3])] * 3) for index in tri[::-1]]
    return merge([side, *caps])


def torus(major=0.6, minor=0.2, rings=24):
    profile = []
    for ring in range(rings + 1):
        theta = 2 * math.pi * ring / rings
        profile.append((major + minor * math.cos(theta), minor * math.sin(theta),
                        math.cos(theta), math.sin(theta)))
    return lathe(profile, 48)


def plane():
    positions = [(-0.5, 0, 0.5), (0.5, 0, 0.5), (0.5, 0, -0.5), (-0.5, 0, -0.5)]
    return positions, [(0, 1, 0)] * 4, [(0, 12), (12, 12), (12, 0), (0, 0)], [0, 1, 2, 0, 2, 3]


def merge(parts):
    positions, normals, uvs, indices = [], [], [], []
    for p, n, u, i in parts:
        base = len(positions)
        positions += p
        normals += n
        uvs += u
        indices += [base + index for index in i]
    return positions, normals, uvs, indices


MESHES = [("Cube", cube()), ("Sphere", sphere()), ("Capsule", capsule()),
          ("Cylinder", cylinder()), ("Plane", plane()), ("Torus", torus())]

# name, base colour (RGBA), metallic, roughness, emissive, alpha mode
MATERIALS = [
    ("Ground", (0.46, 0.48, 0.5, 1), 0.0, 0.92, None, "OPAQUE"),
    ("Brick", (0.78, 0.2, 0.14, 1), 0.0, 0.55, None, "OPAQUE"),
    ("Ocean", (0.12, 0.38, 0.86, 1), 0.0, 0.35, None, "OPAQUE"),
    ("Mint", (0.28, 0.82, 0.58, 1), 0.0, 0.6, None, "OPAQUE"),
    ("Gold", (1.0, 0.77, 0.34, 1), 1.0, 0.24, None, "OPAQUE"),
    ("Chrome", (0.95, 0.95, 0.96, 1), 1.0, 0.06, None, "OPAQUE"),
    ("Copper", (0.95, 0.62, 0.52, 1), 1.0, 0.45, None, "OPAQUE"),
    ("Rubber", (0.05, 0.05, 0.06, 1), 0.0, 0.95, None, "OPAQUE"),
    ("Glass", (0.62, 0.82, 1.0, 0.32), 0.0, 0.08, None, "BLEND"),
    ("Glow", (1.0, 0.55, 0.18, 1), 0.0, 0.4, (1.0, 0.48, 0.12), "OPAQUE"),
]


def build_glb():
    binary = bytearray()
    views, accessors, meshes = [], [], []

    def add(data, target, component, kind, count, bounds=None):
        while len(binary) % 4:
            binary.append(0)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data),
                      "target": target})
        binary.extend(data)
        accessor = {"bufferView": len(views) - 1, "componentType": component, "count": count,
                    "type": kind}
        if bounds:
            accessor["min"], accessor["max"] = bounds
        accessors.append(accessor)
        return len(accessors) - 1

    # Importers drop materials no primitive references, so tiny swatches carry the remainder.
    swatch = ([(0, 0, 0), (0.1, 0, 0), (0, 0.1, 0)], [(0, 0, 1)] * 3, [(0, 0)] * 3, [0, 1, 2])
    all_meshes = MESHES + [("Swatch " + MATERIALS[index][0], swatch)
                           for index in range(len(MESHES), len(MATERIALS))]
    for index, (name, (positions, normals, uvs, indices)) in enumerate(all_meshes):
        bounds = ([min(p[a] for p in positions) for a in range(3)],
                  [max(p[a] for p in positions) for a in range(3)])
        flat = lambda rows: b"".join(struct.pack("<%df" % len(row), *row) for row in rows)
        position = add(flat(positions), 34962, 5126, "VEC3", len(positions), bounds)
        normal = add(flat(normals), 34962, 5126, "VEC3", len(normals))
        uv = add(flat(uvs), 34962, 5126, "VEC2", len(uvs))
        index_data = struct.pack("<%dI" % len(indices), *indices)
        indices_accessor = add(index_data, 34963, 5125, "SCALAR", len(indices))
        meshes.append({"name": name, "primitives": [{
            "attributes": {"POSITION": position, "NORMAL": normal, "TEXCOORD_0": uv},
            "indices": indices_accessor, "material": index}]})

    materials = []
    for name, color, metallic, roughness, emissive, alpha in MATERIALS:
        material = {"name": name, "pbrMetallicRoughness": {
            "baseColorFactor": list(color), "metallicFactor": metallic,
            "roughnessFactor": roughness}}
        if emissive:
            material["emissiveFactor"] = list(emissive)
        if alpha != "OPAQUE":
            material["alphaMode"] = alpha
        materials.append(material)

    document = {
        "asset": {"version": "2.0", "generator": "Relay demo project generator"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(all_meshes)))}],
        "nodes": [{"name": name, "mesh": index, "translation": [index * 1.5, 0, 0]}
                  for index, (name, _) in enumerate(all_meshes)],
        "meshes": meshes, "materials": materials, "accessors": accessors,
        "bufferViews": views, "buffers": [{"byteLength": len(binary)}],
    }
    text = json.dumps(document, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    binary += b"\0" * (-len(binary) % 4)
    chunks = struct.pack("<II", len(text), 0x4E4F534A) + text
    chunks += struct.pack("<II", len(binary), 0x004E4942) + binary
    return struct.pack("<III", 0x46546C67, 2, 12 + len(chunks)) + chunks


def main():
    models = PROJECT / "models"
    models.mkdir(parents=True, exist_ok=True)
    (models / "primitives.glb").write_bytes(build_glb())
    if not BUILDER.exists():
        sys.exit(f"Build the dev preset first; {BUILDER.relative_to(ROOT)} is missing")
    subprocess.run([str(BUILDER), str(PROJECT.relative_to(ROOT))], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
