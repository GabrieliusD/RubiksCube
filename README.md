# Cherry Engine + Rubik's Cube

A small DirectX 12 engine (**CherryEngine**) and a Rubik's cube game built on it.
Like Unreal, the engine and the game are separate projects: the engine is a DLL,
each game is a module DLL, and **CherryLaunch** loads a game module and runs it.

```
CherryLaunch.exe ──loads──▶ RubikGame.dll ──links──▶ CherryEngine.dll
```

## Layout

| Folder | What it is |
|---|---|
| `CherryEngine/Core` | `Engine` (main loop), `Window`, `Input`, logging, camera controllers |
| `CherryEngine/Game` | `IGameModule`: the interface a game implements |
| `CherryEngine/Scene` | ECS, `Scene`, built-in components (`Transform`, `MeshRenderer`, `Camera`, `VoxelVolumeRenderer`) |
| `CherryEngine/RHI` | Thin D3D12 layer: device + frames in flight, bindless descriptor heap, buffers/textures, swap chain, pipeline library with shader hot reload |
| `CherryEngine/Renderer` | Frame orchestration, GPU scene, passes, asset pools (mesh/material/texture/voxel handles), debug draw |
| `CherryEngine/Editor` | ImGui editor layer: dockspace, viewport, stats, game panel |
| `CherryEngine/Shaders` | HLSL (SM 6.6, compiled at runtime by DXC). `Shared/ShaderInterop.h` is used by both C++ and HLSL |
| `Rubik` | The Rubik's cube game module (`RubikModule`, pure-logic `RubikPuzzle`, tests) |
| `Rubik/Content` | Game assets, copied next to the launcher on build |
| `CherryLaunch` | The launcher executable |

## Renderer

Bindless (Shader Model 6.6 `ResourceDescriptorHeap`) with one global root signature,
two frames in flight, and an HDR pipeline:

1. **Mesh pass**: forward Blinn-Phong into HDR colour + depth
2. **Voxel pass**: ray-marched voxel volumes (DDA through a 3D texture) that write `SV_Depth`, so voxels and meshes intersect correctly
3. **Tonemap**: compute shader, HDR to LDR (ACES)
4. **Debug lines**: bounds, picking rays, etc.
5. **Present**: the LDR view is shown in the editor viewport, or blitted full-window in game mode

Requires a GPU/driver with Shader Model 6.6 and resource binding tier 3. The engine
checks this at startup and reports what's missing.

## Building (Windows, Visual Studio 2026 + vcpkg)

The project uses CMake presets and vcpkg manifest mode (`vcpkg.json` pulls in DXC).
Set `VCPKG_ROOT`, or use the vcpkg that ships with Visual Studio.

```bash
cmake --preset x64-debug
cmake --build out/build/x64-debug --config Debug
```

Run `out/build/x64-debug/bin/Debug/CherryLaunch.exe`. Shaders, DXC DLLs and game
content are copied into the same folder on every build.

Build options:

- `-DCHERRY_MONOLITHIC=ON` links the engine and game statically into one executable.
- `-DCHERRY_ACTIVE_GAME_TARGET=<target>` picks which game module the launcher loads.
  You can also pass a module DLL path as the launcher's first argument.

Tests: `ctest --test-dir out/build/x64-debug -C Debug`

Debug builds compile shaders from `CherryEngine/Shaders` in the source tree and
hot-reload them on save. A shader that fails to compile keeps its previous version,
and the error is shown in the Stats panel.

## Controls

| Key | Action |
|---|---|
| Left drag / wheel | Orbit / zoom the camera |
| Right click | Select the face under the cursor |
| E / Q | Turn the selected face clockwise / counter-clockwise |
| R L U D F B | Turn that face (Shift = counter-clockwise) |
| F1 | Toggle editor / full-window game mode |
| F2 | Toggle the voxel test volume |
| F3 | Toggle bounding-box debug lines |

## Writing a game module

```cpp
#include <Game/GameModule.h>
#include <Core/Engine.h>

class MyGame : public IGameModule
{
public:
    void OnStartup(Engine& engine) override { /* create meshes, materials, entities */ }
    void OnUpdate(Engine& engine, float dt) override { /* game logic */ }
    void OnImGui(Engine& engine) override { /* game UI */ }
};

CHERRY_IMPLEMENT_GAME_MODULE(MyGame)
```

Build it as a shared library that links `CherryEngine`, then point
`CHERRY_ACTIVE_GAME_TARGET` at it.
