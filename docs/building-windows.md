# Building Relay on Windows

Relay builds on Windows with Visual Studio 2022. The editor, the player and the headless engine use
Vulkan and SDL3, so no DirectX is involved. Linux is the platform Relay is developed and tested on;
the Windows build compiles in CI but has had less use, so please report anything that behaves
differently.

## What to install

| Tool | Why | Notes |
| --- | --- | --- |
| [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community is enough) | C++20 compiler and the Windows SDK | Install the **Desktop development with C++** workload. |
| [Git](https://git-scm.com/download/win) | Fetching the source | |
| [CMake](https://cmake.org/download/) 3.25 or newer | Build system | Visual Studio's bundled CMake also works. |
| [Python](https://www.python.org/downloads/) 3.10 or newer | Code generation during the build | Tick **Add python.exe to PATH**. |
| [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows) | Vulkan headers and loader, `glslc` and `glslangValidator` | The installer sets `VULKAN_SDK` and adds `Bin` to `PATH`. Open a new terminal afterwards. |

You also need a GPU driver with Vulkan 1.3 support. Ray traced lighting needs a card that supports
ray queries.

SDL3 and the other libraries Relay needs (Jolt, miniaudio, Dear ImGui and AMD FidelityFX) are
downloaded and built by CMake, so there is nothing else to install. The first configure needs an
internet connection.

## Build

Open **x64 Native Tools Command Prompt for VS 2022** (or any terminal, as the presets pick the Visual
Studio generator), then:

```bat
git clone https://github.com/patrick123900/relay-engine.git
cd relay-engine
cmake --preset windows
cmake --build --preset windows --parallel
build\windows\relay_demo.exe
```

`build\windows` holds `relay_demo.exe` (the editor), `relay_player.exe` (the program a built game
runs) and the compiled `shaders` folder. Use `--preset windows-debug` for a Debug build. Run the
editor from that folder or by its path; it looks for the player and its shaders beside itself.

`relay_demo.exe --headless` runs without a window, and `relay_demo.exe --editor` opens the editor.

### Optional libraries

Without them the build still succeeds, with some features turned off (CMake prints what was left out):

| Library | Feature it enables |
| --- | --- |
| libpng, libjpeg-turbo | Importing and decoding PNG and JPEG textures |
| Assimp | Importing models (glTF, FBX, OBJ and others) |
| glslang | Compiling project shaders |

The simplest way to get them is [vcpkg](https://github.com/microsoft/vcpkg). Relay includes a
`vcpkg.json` manifest and a preset that uses it:

```bat
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
set VCPKG_ROOT=C:\vcpkg
cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg --parallel
```

The first configure builds these libraries, which takes a while. They are linked statically, so no
extra DLLs have to sit beside the programs.

### Tests

The `windows` preset leaves tests off. To build and run the engine tests:

```bat
cmake --preset windows -DRELAY_BUILD_TESTS=ON
cmake --build --preset windows --parallel
ctest --test-dir build\windows -C Release --output-on-failure -R "relay_tests|relay_workflow_tests"
```

Some tests drive the editor window or compile gameplay scripts and are not expected to pass on
Windows yet.

## Building and running games

**Run > Run standalone game** and **Run > Build project...** work on Windows and make a
`windows x86_64` game from the `relay_player.exe` beside the editor. Targets for other platforms
need a player installed under **Run > Manage players...** (see [portable-builds.md](portable-builds.md)).

## Known limitations

- **Gameplay scripts are not supported on Windows.** Projects that depend on native scripts load,
  but their scripts report as unsupported (see [scripting.md](scripting.md)).
- **Other tools:** the `relay_elf_requirements` helper only reads Linux programs, and the glibc
  settings in the Build window do not apply.
- **Visual Studio only.** Other Windows compilers are not tried. If you build with MinGW, use the
  Ninja generator and a Vulkan SDK import library for it.
- **Not much runtime testing.** The renderer is verified on Linux with Mesa's RADV driver; Windows
  drivers may expose differences.

## Troubleshooting

- **`Vulkan window disabled: Vulkan loader or glslc was not found`**: the Vulkan SDK is not
  installed, or the terminal was opened before its installer ran. Open a new terminal and check
  that `echo %VULKAN_SDK%` prints a folder.
- **`FidelityFX disabled: glslangValidator was not found`**: same cause; the SDK's `Bin` folder must
  be on `PATH`.
- **`Python3 ... not found`**: install Python 3.10 or newer with *Add python.exe to PATH*.
- **Slow first configure**: SDL3 and the optional libraries are built from source once and cached
  in `build\`.
- **Vulkan errors at startup**: update the graphics driver.
