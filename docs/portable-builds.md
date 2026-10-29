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

## Other targets

Builds for a platform or architecture other than the one running the editor use a *player built on that
target*. The editor looks for it in `players/<platform>-<architecture>/` (for example
`players/windows-x86_64/` or `players/linux-aarch64/`) beside the editor, or in the folder named by
`RELAY_PLAYERS_DIR`. A target's folder holds the program (`relay_player` or `relay_player.exe`), a
`shaders/` folder and any libraries it needs (`lib/` on Linux, or `.dll`/`.dylib` files beside the
program), and is copied into the build as is. `project.build_settings` lists each target and whether
its player is installed (`targets`), and a build for a target with none says where to put one.

Not done yet:

- Relay itself has only been built on Linux x86_64. No Windows, macOS or Arm player is produced.
- A project with scripts cannot be built for another target, because scripts are compiled to native
  code on the editor's machine (`.so`). Such a build is refused. Cross-compiling scripts, or building
  them on the target, is open.
- Windows and macOS packaging beyond copying the player (icons, bundles, signing) is not started.
