# Portable game builds

A built game needs three things from the machine it runs on: a Vulkan driver, the C library (glibc)
and the C++ runtime. Relay bundles libstdc++ in the build's `lib/`; it never copies glibc or the GPU
driver, because both must match the machine. That makes glibc the one thing that limits where a build
runs: **a build needs a glibc at least as new as the one it was linked against, which is the build
machine's.**

## Measuring the floor

Every build records what the player, the libraries it ships and the compiled scripts (`scripts/relay-scripts.so`, built on the editor's machine) ask of the system, in `build.json`
under `requirements` (glibc, the library that set it, libstdc++ and libgcc versions), and the Build
window and `project.build` report the same. You can check any file yourself:

```bash
build/dev/relay_elf_requirements path/to/Game_Name path/to/lib/*.so*
build/dev/relay_elf_requirements --max-glibc 2.31 path/to/Game_Name
```

The second form exits 1 if a file needs a newer glibc than the ceiling, which suits a CI step.

## Insisting on a minimum

A profile's **Minimum glibc** (`minimum_glibc`; empty, the default, means no check; `2.31` is a good value once you have an old-glibc player) makes the build
fail, after measuring, when the player or a shipped library needs more. The failed build leaves the
previous one in place. The setting does not make the player older; it only stops a build that would not
run where you intend to ship it.

## Building a player for older systems

The measured floor comes from the machine that compiled `relay_player`, so to reach older systems
compile Relay on an older one: a container or chroot of an older distribution (any release whose glibc
is the oldest you want to support), with Vulkan headers, SDL3 and the other dependencies from the
README. Build the `relay_player` target there, then check it with `relay_elf_requirements --max-glibc`.
The result is installed like any player, below. No such environment is provided or tested yet.

## Players and other targets

Builds are made from a *player*, a program built for one platform and architecture. Relay offers a build for
each target it has a player for: the player built into Relay for the machine's own Linux target, and any player
installed below the `players/` folder beside the editor (or in the folder named by `RELAY_PLAYERS_DIR`). **Run
→ Manage players...** adds and removes them (`players.list`, `players.install` and `players.remove` in the
protocol; installing and removing are for the person at the editor, not agents). An installed player for the
machine's own target is used instead of the built-in one, which is how an old-glibc Linux player is put to use.

A player is a folder, `players/<platform>-<architecture>/`, holding:

- `player.json`, which says what it is: `{"format": "relay.player", "version": 1, "platform": "windows",
  "architecture": "x86_64", "program": "relay_player.exe", "name": "Windows (x86_64)", "description": "...",
  "version": "1.0"}`. Only `format`, `platform`, `architecture` and `program` are needed. A folder without one is
  named `<platform>-<architecture>` and holds `relay_player` or `relay_player.exe`; installing it from the
  editor asks which target it is for and writes a `player.json`.
- the program, which a build copies and names after the game, keeping its extension;
- a `shaders/` folder of compiled `.spv` files, and every other file the player needs (`lib/` on Linux,
  `.dll` or `.dylib` files beside the program), which are copied into the build as they are.

Platform and architecture are short lower-case words (`windows`, `macos`, `linux`, `android`; `x86_64`,
`aarch64`, `arm64`), not a fixed list, so a player made by someone else for another target is built for like
any other. Relay itself has only been built on Linux x86_64 so far.

Not done yet:

- No Windows, macOS or Arm player is produced here, and players cannot yet be downloaded from a URL (the Add
  player window has the field, disabled).
- A project with scripts cannot be built for another target, because scripts are compiled to native code on
  the editor's machine (`.so`). Such a build is refused. Cross-compiling scripts, or building them on the
  target, is open.
- Packaging beyond copying the player (Windows icons, macOS bundles, signing, Android packages) is not started.
