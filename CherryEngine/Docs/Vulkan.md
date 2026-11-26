# Vulkan backend (Windows): plan and status

Goal: everything the D3D12 renderer draws also draws on Vulkan, and the editor switches between
the two. Branch `features/vulkan` (from `features/android`).

Decided with the user (2026-11-26):
- **One renderer, two backends.** `RHI/` becomes an interface with a D3D12 and a Vulkan
  implementation. `Renderer.cpp`'s passes and every HLSL shader (bindless, surface shaders,
  shader graphs, hot reload) are shared, so a new feature works on both.
- **Windows first.** Android keeps its own mobile renderer (`Renderer/Vulkan/VulkanRenderer.cpp`)
  for now: phones rarely have `VK_EXT_mutable_descriptor_type` and have no DXC at run time, so the
  shared renderer on Android needs a shader cook step and a non-bindless fallback (a later step).
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
| V3 | Switching in the editor | Edit > Graphics API (D3D12 / Vulkan) restarts into the same project; the choice is remembered; the stats show the API. Docs updated. | Not started |
