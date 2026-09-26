#!/usr/bin/env python3
"""Regenerate the dev-build demo project in examples/demo.

Writes examples/demo/models/primitives.glb (unit primitives and a small PBR material library) and
the synthesised sounds in examples/demo/sounds, then runs the native builder, which imports it through the control protocol and authors the showcase
scene. Run from the repository root after building the dev preset:

    python3 tools/generate_demo_project.py
"""
import json
import math
import random
import pathlib
import struct
import subprocess
import sys
import wave

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


SAMPLE_RATE = 48000


def write_wav(path, samples, rate=SAMPLE_RATE):
    """Mono 16-bit PCM; samples are floats in [-1, 1], scaled to peak at 0.9."""
    peak = max(1e-9, max(abs(value) for value in samples))
    scale = min(1.0, 0.9 / peak) * 32767
    frames = b"".join(struct.pack("<h", round(value * scale)) for value in samples)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(frames)


def tone():
    """A soft bell at C5: a few inharmonic partials that ring out over a second."""
    samples = []
    for index in range(int(SAMPLE_RATE * 1.2)):
        time = index / SAMPLE_RATE
        attack = min(1.0, time / 0.004)
        value = 0.0
        for ratio, level, decay in ((1.0, 1.0, 2.8), (2.0, 0.35, 4.5), (2.76, 0.18, 6.0),
                                    (5.4, 0.06, 9.0)):
            value += level * math.exp(-decay * time) * math.sin(2 * math.pi * 523.25 * ratio * time)
        samples.append(attack * value)
    return samples


def thump():
    """A short low knock: a falling sine with a burst of filtered noise at the start."""
    generator = random.Random(0x7E1A)
    samples, noise = [], 0.0
    for index in range(int(SAMPLE_RATE * 0.35)):
        time = index / SAMPLE_RATE
        # The pitch falls from 150 Hz to 55 Hz; this is the integral of that frequency.
        phase = 2 * math.pi * (55 * time + 95 * (1 - math.exp(-time * 30)) / 30)
        noise += 0.25 * (generator.uniform(-1, 1) - noise)
        body = math.sin(phase) * math.exp(-time * 14)
        click = noise * math.exp(-time * 90) * 0.6
        samples.append(min(1.0, time / 0.001) * (body + click))
    return samples


def hum():
    """A two-second drone that loops seamlessly: every partial completes whole cycles."""
    duration = 2.0
    samples = []
    for index in range(int(SAMPLE_RATE * duration)):
        time = index / SAMPLE_RATE
        swell = 0.75 + 0.25 * math.sin(2 * math.pi * 0.5 * time)
        value = (math.sin(2 * math.pi * 110 * time) + 0.5 * math.sin(2 * math.pi * 165 * time) +
                 0.3 * math.sin(2 * math.pi * 220.5 * time) + 0.15 * math.sin(2 * math.pi * 330 * time))
        samples.append(swell * value)
    return samples


def shot():
    """A short "pew": a sine sweeping down from 1.4 kHz with a breath of noise."""
    generator = random.Random(0x5407)
    samples, phase = [], 0.0
    for index in range(int(SAMPLE_RATE * 0.22)):
        time = index / SAMPLE_RATE
        frequency = 300 + 1100 * math.exp(-time * 22)
        phase += 2 * math.pi * frequency / SAMPLE_RATE
        envelope = min(1.0, time / 0.002) * math.exp(-time * 16)
        samples.append(envelope * (math.sin(phase) + 0.15 * generator.uniform(-1, 1)))
    return samples


MUSIC_RATE = 24000
BEAT = 0.5  # 120 BPM.


def note(semitones_from_a4):
    return 440.0 * 2 ** (semitones_from_a4 / 12)


def track(chords, arpeggio):
    """Eight bars at 120 BPM in 4/4 (16 s): a soft pad on each chord (two bars each), a plucked
    bass on beats one and three, and an eighth-note arpeggio. Chords are lists of semitones from
    A4; arpeggio indexes into each chord."""
    duration = 16.0
    samples = [0.0] * int(MUSIC_RATE * duration)
    chord_seconds = 4 * BEAT * 2
    for number, chord in enumerate(chords):
        start = number * chord_seconds
        # Pad: the chord's tones an octave down, swelling in and out over its two bars.
        for tone in chord:
            frequency = note(tone - 12)
            for index in range(int(start * MUSIC_RATE), int((start + chord_seconds) * MUSIC_RATE)):
                time = index / MUSIC_RATE - start
                swell = math.sin(math.pi * time / chord_seconds) ** 0.6
                samples[index] += 0.08 * swell * (math.sin(2 * math.pi * frequency * index / MUSIC_RATE) +
                                                  0.3 * math.sin(4 * math.pi * frequency * index / MUSIC_RATE))
        # Bass on beats one and three, two octaves below the root.
        for beat in range(0, 8, 2):
            onset = start + beat * BEAT
            frequency = note(chord[0] - 24)
            for index in range(int(onset * MUSIC_RATE), int((onset + 0.9) * MUSIC_RATE)):
                time = index / MUSIC_RATE - onset
                samples[index] += 0.35 * math.exp(-time * 4) * min(1.0, time / 0.005) * \
                    math.sin(2 * math.pi * frequency * time)
        # Arpeggio on every eighth note.
        for step in range(16):
            onset = start + step * BEAT / 2
            # Degrees past the chord's last tone climb an octave.
            degree = arpeggio[step % len(arpeggio)]
            frequency = note(chord[degree % len(chord)] + 12 * (degree // len(chord)))
            for index in range(int(onset * MUSIC_RATE), int((onset + 0.24) * MUSIC_RATE)):
                time = index / MUSIC_RATE - onset
                wave = 2 / math.pi * math.asin(math.sin(2 * math.pi * frequency * time))  # Triangle.
                samples[index] += 0.12 * math.exp(-time * 12) * min(1.0, time / 0.003) * wave
    return samples


def main():
    models = PROJECT / "models"
    models.mkdir(parents=True, exist_ok=True)
    (models / "primitives.glb").write_bytes(build_glb())
    sounds = PROJECT / "sounds"
    sounds.mkdir(parents=True, exist_ok=True)
    write_wav(sounds / "tone.wav", tone())
    write_wav(sounds / "thump.wav", thump())
    write_wav(sounds / "hum.wav", hum())
    write_wav(sounds / "shot.wav", shot())
    music = PROJECT / "music"
    music.mkdir(parents=True, exist_ok=True)
    # C major (C, Am, F, G) and its darker turn (Am, F, C, G); semitones from A4.
    c, a_minor, f, g = [3, 7, 10], [0, 3, 7], [-4, 0, 3], [-2, 2, 5]
    write_wav(music / "daylight.wav", track([c, a_minor, f, g], [0, 1, 2, 3, 2, 1]), MUSIC_RATE)
    write_wav(music / "dusk.wav", track([a_minor, f, c, g], [0, 2, 1, 3, 4, 3, 1, 2]), MUSIC_RATE)
    if not BUILDER.exists():
        sys.exit(f"Build the dev preset first; {BUILDER.relative_to(ROOT)} is missing")
    subprocess.run([str(BUILDER), str(PROJECT.relative_to(ROOT))], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
