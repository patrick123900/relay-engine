# AMD FidelityFX SDK (subset)

Relay's global illumination and reflection denoising come from the
[AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), MIT licensed
(see [`LICENSE.txt`](LICENSE.txt)).

- Version: tag `v1.1.4`, the last release with a Vulkan backend (2.x is DirectX 12 only).
- Source archive: `https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v1.1.4.tar.gz`,
  SHA-256 `25c46398a656150397597f78d44bf7cb445e9e177dd116b309bbfdf50d50cc9f`.

The archive is about 200 MB, mostly samples, media and Windows binaries, so only the parts Relay
builds are kept here, in the upstream directory layout:

| Path | Contents |
| --- | --- |
| `sdk/include/FidelityFX/host` | Public headers for the effects below, and the Vulkan backend header |
| `sdk/include/FidelityFX/gpu` | Shader sources for Brixelizer, Brixelizer GI, Denoiser, Classifier and SPD |
| `sdk/src` | Effect host code, the shared backend code, the Vulkan backend and blob accessors |
| `tools/ffx_shader_compiler` | The shader permutation compiler (`FidelityFX_SC`), with MD5, SPIRV-Reflect and tiny-process-library |

`CMakeLists.txt` and `relay/` are Relay's own. Upstream builds only with Visual Studio, and its
shader compiler only on Windows. `CMakeLists.txt` builds the compiler for the host, runs it over
every GLSL pass (wave32 and wave64, each with and without 16-bit types), and builds a static
library, `relay_fidelityfx`.

## Local changes

[`relay/local-changes.diff`](relay/local-changes.diff) is the complete diff against v1.1.4. Every
change is marked with a `Relay:` comment.

- The shader compiler builds and runs outside Windows: Windows-only headers and the HLSL compilers
  are behind `_WIN32`, paths and files use `std::filesystem` and `fopen`, arguments arrive as UTF-8,
  `glslangValidator` is found without `.exe`, and compiler messages keep their last character.
  It also accepts `@file` response files. A POSIX shell would otherwise brace-expand permutation
  lists such as `-DOPTION={0,1}` into separate arguments.
- Headers include what they use (`<bit>`, `<math.h>`, `<locale>`). MSVC's headers pull these in
  transitively, GCC's and Clang's do not.
- Opaque context sizes (`FFX_*_CONTEXT_SIZE`, `FFX_BRIXELIZER_UPDATE_DESCRIPTION_SIZE`) are larger
  outside Windows. The private contexts embed `wchar_t` resource names, and `wchar_t` is 4 bytes
  there instead of 2. The Brixelizer baked-update check is `>=` rather than `==` for the same
  reason.
- The Vulkan backend falls back to the core `vkGetBufferMemoryRequirements2` when the application
  has not enabled `VK_KHR_get_memory_requirements2`, and it no longer references the frame
  generation swapchain, which is not built.
- The Vulkan backend aligns every array in its scratch memory to `alignof(std::max_align_t)`.
  Upstream packs them at 4 bytes, and optimized GCC builds then crash with an aligned store into
  a misaligned effect context.
- Fixes found by the Khronos validation layer:
  - The Vulkan backend's global descriptor pool lists each descriptor type once, including
    storage buffers, which upstream left out.
  - Its bindless pool allows freeing individual sets, which the backend does when it is
    destroyed.
  - Two classifier images are declared in the formats the reflection denoiser gives them:
    radiance as `rgba16f`, and extracted roughness as `r8`, since the denoiser copies it into an
    R8 history image.
- `relay/ffx_relay_compat.h`, force-included into the library outside Windows, supplies
  `_countof`, `wcscpy_s`, `strcpy_s`, `sprintf_s`, `swprintf_s` and `wcstombs_s`.

`tests/fidelityfx_tests.cpp` creates each effect on a headless Vulkan device, which compiles all
of its pipelines from the generated SPIR-V and reflection data.
