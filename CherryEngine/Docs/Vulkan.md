# Vulkan backend (Windows): plan and status

Goal: everything the D3D12 renderer draws also draws on Vulkan, and the editor switches between
the two. Branch `features/vulkan` (from `features/android`).

Decided with the user (2026-11-26):
- **One renderer, two backends.** `RHI/` becomes an interface with a D3D12 and a Vulkan
  implementation. `Renderer.cpp`'s passes and every HLSL shader (bindless, surface shaders,
  shader graphs, hot reload) are shared, so a new feature works on both.
- **Windows first, then Android** (asked for on 2026-11-26, milestones S1-S3 below): Android now
  runs the same renderer; its separate mobile renderer is gone.
- **Switching:** a Graphics API choice in the editor (and `--rhi d3d12|vulkan` on the command
  line), saved per user; changing it restarts the editor into the same project.

## How the shaders map to Vulkan

DXC compiles the same HLSL to SPIR-V (`-spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout
-fspv-use-unknown-image-format`):
- `ResourceDescriptorHeap[i]` → set 1, binding 0 (`-fvk-bind-resource-heap 0 1`): one array of
  `VK_DESCRIPTOR_TYPE_MUTABLE_EXT` descriptors, indexed by the same bindless index as on D3D12.
- `b0` (per-draw constants) → push constants (`CHERRY_PUSH_CONSTANTS` on `gDraw`); `b1`/`b2`
  (view, frame) → set 0 bindings 1/2, dynamic uniform buffers in the constant ring; `s0..s6` →
  set 0 bindings 16..22, immutable samplers.
- Clip space: a negative viewport height gives D3D's orientation, so no shader changes.
- Vertex inputs: `CHERRY_LOCATION(n)` (`[[vk::location(n)]]` on SPIR-V) on the vertex shader inputs.
- Entry points are renamed `main` (`-fspv-entrypoint-name`); cbuffers and structured buffers keep
  D3D's layout (`-fvk-use-dx-layout`, with `scalarBlockLayout` on the device).
- `discard` needs `shaderDemoteToHelperInvocation` (Vulkan 1.3).

Checked on this PC (RTX 4070, Vulkan 1.4): mutable descriptors, descriptor indexing, dynamic
rendering, synchronization2, timeline semaphores, storage images without format.

## Milestones

| # | Milestone | Done when | Status |
|---|---|---|---|
| V1 | Backend-neutral RHI, D3D12 behind it | `Renderer.cpp` uses only the neutral API (command list, textures, buffers, pipelines, swap chain, timestamps); ImGui drawn by the renderer itself (no `imgui_impl_dx12`); DDS read by the engine. D3D12 screenshots of Dice and UNDERTIME match the old build, tests pass, frame times unchanged. | Done (2026-11-26): `RHI/RHI.h` (device, command list, buffers, textures, pipelines), `RHI/D3D12/D3D12Device.cpp`, `Renderer/ImGuiRenderer.cpp`, `Renderer/DdsReader.cpp`. Dice and UNDERTIME (valley, cave mouth) screenshots match the old build, no debug-layer messages, 144 fps either way (vsync), the four test suites pass, the editor draws its view. |
| V2 | Vulkan backend | `--rhi vulkan` runs Dice, Rubik and UNDERTIME with every pass (shadows, voxels, sky, particles, SSAO, bloom, light shafts, edges, eye adaptation, FXAA, previews, the editor); surface shaders and shader graphs compile and hot-reload; validation layers report nothing; screenshots match D3D12. | Done (2026-11-26): `RHI/Vulkan/VulkanDevice.cpp` (volk + VMA from vcpkg, Vulkan 1.3, dynamic rendering, synchronization2, timeline semaphores). `--rhi vulkan` or `CHERRY_RHI=vulkan`. Dice, Rubik and UNDERTIME (valley by day, at night and in rain, the cave mouth, the showcase cave, a conversation with its portrait) match the D3D12 screenshots (mean difference under 0.5/255; the rest is particles moving between runs); UNDERTIME's GPU time is 2.45 ms on Vulkan against 2.7 ms on D3D12 in the valley. Validation clean in Debug (the installed SDK's layers, 1.3.216, can't check mutable descriptors; a newer SDK can). The editor works on Vulkan (resize, minimize, close), shader hot reload rebuilds as SPIR-V, and the tests compile every engine shader and shader graph node for both targets. |
| V3 | Switching in the editor | Edit > Graphics API (D3D12 / Vulkan) restarts into the same project; the choice is remembered; the stats show the API. Docs updated. | Done (2026-11-26): Edit > Graphics API, driven by clicks: D3D12 to Vulkan and back, each time restarting on the same project and scene; the Stats panel names the API and counts its validation messages. If Vulkan can't start, the editor says so and runs on D3D12 (and remembers that). |

## Android on the shared renderer

| # | Milestone | Status |
|---|---|---|
| S1 | Shaders cooked to SPIR-V on Windows, read from a cache where there's no DXC | Done: `CherryLaunch <project> --cook-shaders <dir>` plays the project ~6 s, loads every surface shader and graph in Content (with voxel and compact variants) and writes the SPIR-V of everything compiled (`ShaderSystem/ShaderCache.h`: keyed by stage, entry, defines, file and generated source). Android's `ShaderSystem/ShaderCompiler_Cooked.cpp` reads it; `CHERRY_SHADER_CACHE=<dir>` does the same on Windows. Dice cooks 32-39 shaders, UNDERTIME 105; both run from the cache alone on Vulkan and match D3D12. SPIR-V targets Vulkan 1.1. |
| S2 | The Vulkan backend on phone drivers | Done: Vulkan 1.1-1.3 with features from core or extensions; Android surfaces (lost and recreated with the app); without mutable descriptors the heap is three typed arrays (shaders' heap bindings rewritten at load); without dynamic rendering, render passes and framebuffers; without synchronization2, Vulkan 1.0 barriers and submits; fences instead of timeline semaphores (the emulator advertises timeline semaphores without the functions). `CHERRY_VK_NO_MUTABLE=1` and `CHERRY_VK_LEGACY=1` force those paths on a desktop GPU: the showcase cave matches D3D12 with no validation errors either way. |
| S3 | Android builds the shared renderer | Done: `Renderer.cpp`, the RHI and the Vulkan backend build for Android (volk and VMA from vcpkg); `Renderer/Vulkan/VulkanRenderer.cpp` and `Shaders/Mobile` are deleted. `Tools/Android/Package.ps1` cooks with the Windows build (`-CookExe`; the editor's export passes itself) and ships `ShaderCache/` in the APK. On the emulator (Android 12, x86_64): Dice plays at 60 fps through the fallbacks (typed arrays, render passes, 1.0 barriers), Home and back recreates the surface, a tap starts a session. |

## Not done / next

- A real phone: the user's phone ran the old mobile renderer; the shared renderer needs a fresh
  test there (`Tools/Android/Package.ps1 -Project Dice/Dice.cherryproject -Config Release -Install`).
  Phones without `shaderStorageImageWriteWithoutFormat` are refused with a message (the post
  passes write storage images without a declared format).
- Storage images in R8 (SSAO, edge highlights) need `shaderStorageImageExtendedFormats`; Dice
  doesn't use those passes, UNDERTIME would on a phone that lacks it.
- A newer Vulkan SDK on this PC: the installed validation layers (1.3.216) can't check the
  mutable-descriptor heap, and report a false "previously bound pipeline" error that the
  backend filters for layers older than 1.3.250.
- Switching without a restart (rebuilding every GPU resource in place) was not chosen.
