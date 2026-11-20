# Cherry Engine + Rubik's Cube

A small DirectX 12 engine (**CherryEngine**) and a Rubik's cube game built on it.
Like Unreal, the engine and the game are separate projects: the engine is a DLL,
each game is a module DLL, and **CherryLaunch** opens a game project in the editor.
Each game is a project: a `.cherryproject` file next to its code, naming its module
and its `Content/` folder.

```
CherryLaunch.exe ──loads──▶ RubikGame.dll ──links──▶ CherryEngine.dll
```

## Layout

| Folder | What it is |
|---|---|
| `CherryEngine/Core` | `Engine` (main loop, play mode), projects, the module host (loads and hot-reloads the game DLL), background builds, `Window`, `Input`, logging, camera controllers |
| `CherryEngine/Game` | `IGameModule`: the interface a game implements |
| `CherryEngine/Scene` | ECS, `Scene`, built-in components (`Transform`, `MeshRenderer`, `Camera`, `VoxelVolumeRenderer`) |
| `CherryEngine/RHI` | Thin D3D12 layer: device + frames in flight, bindless descriptor heap, buffers/textures, swap chain, pipeline library with shader hot reload |
| `CherryEngine/Renderer` | Frame orchestration, GPU scene, passes, asset pools (mesh/material/texture/voxel handles), debug draw |
| `CherryEngine/Editor` | ImGui editor layer: menu bar with play controls, dockspace, viewport, stats, output log, game panel, project picker, shader graph editor |
| `CherryEngine/ShaderGraph` | Shader graphs: the graph model and file format, the node library, the graph-to-HLSL compiler |
| `CherryEngine/Shaders` | HLSL (SM 6.6, compiled at runtime by DXC). `Shared/ShaderInterop.h` is used by both C++ and HLSL |
| `Rubik` | The Rubik's cube game module (`RubikModule`, pure-logic `RubikPuzzle`, tests) |
| `Rubik/Rubik.cherryproject` | The Rubik project file |
| `Rubik/Content` | Game assets, read from here by the editor (copied next to the exe only in monolithic builds) |
| `CherryLaunch` | The launcher executable |

## Renderer

Bindless (Shader Model 6.6 `ResourceDescriptorHeap`) with one global root signature,
two frames in flight, and an HDR pipeline:

1. **Mesh pass**: forward PBR (metallic/roughness, GGX) into HDR colour + depth. Each material's surface shader is a pipeline of its own; translucent surfaces draw after the voxel pass, back to front
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

Run `out/build/x64-debug/bin/Debug/CherryLaunch.exe`. Shaders and DXC DLLs are copied
into the same folder on every build.

| Command line | What opens |
|---|---|
| *(none)* | The last project, or the project picker |
| `Rubik/Rubik.cherryproject` | That project in the editor |
| `Undertime/Undertime.cherryproject --play` | That project, playing at once (quitting the game closes the window) |
| `UndertimeGame.dll` | Legacy: that module's project, playing at once |

In the editor, **Play** (F5) runs the game in a fresh play world and **Stop** (F5)
throws it away. **Compile** (Ctrl+Shift+B) builds the project's module with CMake in
the background; the editor loads a copy of the game DLL, so the build never hits a
locked file, and any new build (from the editor or from Visual Studio) reloads
automatically. A reload during play stops, reloads and plays again. Changing engine
code still needs an editor restart. Each project keeps its editor state in
`<project>/Saved/` (not checked in).

Build options:

- `-DCHERRY_MONOLITHIC=ON` links the engine and game statically into one executable.
- `-DCHERRY_ACTIVE_GAME_TARGET=<target>` picks the game the monolithic executable runs.

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
| F5 | Play / Stop |
| Ctrl+Shift+B | Compile the game module |

Editor (scene editing, while not playing):

| Key / mouse | Action |
|---|---|
| Right mouse held + WASD / QE | Fly the editor camera (wheel: speed, Shift: faster) |
| Wheel / middle drag | Dolly / pan |
| Left click | Select (meshes, light and camera markers) |
| W / E / R | Move / rotate / scale gizmo (Ctrl flips snapping) |
| F | Frame the selection |
| Ctrl+D / Del | Duplicate / delete (with children) |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Ctrl+N / Ctrl+O / Ctrl+S | New / open / save scene |
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

Build it as a shared library that links `CherryEngine` and call
`cherry_register_game_module(<target>)`. Add a project file next to it:

```json
{ "name": "My Game", "module": "MyGameTarget", "content": "Content" }
```

Load assets with `engine.ContentPath(L"Textures/foo.dds")`, or through `engine.GetAssets()`
by Content path. `OnStartup` runs on every Play (in a fresh copy of the edit scene) and
`OnShutdown` on every Stop, so a game must free everything it created there.

### Scenes, components and systems

Scenes are JSON files (`Content/Scenes/*.scene`) edited in the editor. A game adds its
own components and systems in `RegisterTypes`; they show up in the inspector's Add
Component menu and are saved with the scene:

```cpp
struct Lantern { float intensity = 4.0f; DirectX::XMFLOAT3 color{ 1, 0.8f, 0.6f }; };

void MyGame::RegisterTypes(GameTypes& types)
{
    types.Component<Lantern>("Lantern")
        .Field("intensity", &Lantern::intensity, 0.0f, 20.0f)
        .Field("color", &Lantern::color, Field_Color);
    types.System<LanternSystem>();   // an ISystem: OnPlayBegin / OnUpdate / OnPlayEnd, optional edit-mode hooks
}
```

### Scripts

A script is a component with behaviour. Subclass `Script` (`Game/Script.h`), register it,
and add it to entities in the inspector:

```cpp
class Door : public Script
{
public:
    float openAngle = 90.0f;        // reflected below: edited in the inspector, saved
    void OnStart() override { /* ... */ }
    void OnUpdate(float dt) override { GetTransform().SetRotationEuler(0, ..., 0); }
private:
    float mAngle = 0.0f;            // runtime only
};

types.Script<Door>("Door").Field("openAngle", &Door::openAngle, 0.0f, 180.0f);
// A script deriving from another registered one inherits its fields: .Base<Pawn>()
```

While playing, `OnStart` runs on a script's first frame (also for spawned entities),
`OnUpdate` every frame after the game's `OnUpdate`, and `OnDestroy` when it is destroyed
with `Destroy()` or play stops. Helpers: `GetTransform()`, `Get<T>()`, `Has<T>()`,
`Spawn(prefab, position)`, `Destroy()`, `Find(name)`. Built-in scripts: `Spinner`,
`Spawner` and `Lifetime`.

**New Script** (Content panel) writes `<game>/Scripts/<Name>.h` and `.cpp` from a template,
compiles and reloads; the script then shows up in Add Component. Files in a game's
`Scripts/` folder are built automatically (no CMakeLists edit) and register themselves:

```cpp
CHERRY_SCRIPT(Crystal, "Crystal")      // in Crystal.cpp; include <Game/GameModule.h>
{
    script.Field("bobHeight", &Crystal::bobHeight, 0.0f, 2.0f);
}
```

**Edit Script** in the inspector opens a self-registered script's source file.

### Prefabs

A `.prefab` file is an entity and its children, saved from the hierarchy (right-click,
Save as Prefab...). Drag it from Content into the viewport, or spawn it from code with
`engine.Spawn("Prefabs/Door.prefab", position)`. A placed prefab is a copy: "Save to
Prefab" in the inspector writes an edited instance back to the file, and instances
already placed keep their own copy.

In `OnStartup` (or a system), find placed entities with `scene.Each<Lantern>(...)`.
Fields are matched by name when loading, so changing a component keeps old scenes
loadable, and a hot reload keeps the edit scene through the change. While playing, a
hot reload restarts the game; `SaveState` / `LoadState` let it keep what matters.

## Materials, surface shaders and shader graphs

Lighting is physically based (GGX, metallic/roughness). What a material looks like at each
pixel is decided by its **surface shader**: an HLSL function the engine wraps with the
vertex shader, lighting and fog.

```hlsl
SurfaceOutput EvaluateSurface(SurfaceInput input)
{
    SurfaceOutput s = DefaultSurfaceOutput(input);   // the material's colour, texture, roughness, metallic
    s.emissive = ParamFloat3(input, 0) * pow(1.0 - saturate(dot(input.normalW, input.viewDirW)), 3.0);
    return s;
}
```

- **Built-in:** a `.mat` without a shader uses `Shaders/Mesh.hlsl` (colour x texture).
- **Hand-written:** an `.hlsl` file in Content (Content panel, **New Shader**). A `.mat` picks it
  with `"shader"` and passes values with `"params"` (read with `ParamFloat*`).
- **Shader graphs:** a `.shadergraph` file (**New Shader Graph**, double-click to edit). Nodes
  for inputs, constants, parameters, texture samples, math, vectors, noise, fresnel, normal
  maps, custom HLSL and subgraphs, wired into a Surface Output (base colour, metallic,
  roughness, specular, normal, emissive, opacity, opacity mask, world position offset).
  Blend modes: Opaque, Masked, Translucent.
- **Material instances:** a `.mat` whose shader is a graph shows the graph's parameters in the
  inspector and overrides only what it sets (`"parameters": { "Tint": [1, 0, 0, 1], "Albedo": "Textures/rock.png" }`).
  Materials sharing a graph share one pipeline; their values live in a per-material
  parameter block.

The graph editor recompiles a moment after each edit, and every material using the graph
updates at once. It also shows errors on the nodes, a live preview (drag to turn it) and
the generated HLSL (also written to `Saved/ShaderCache/`). Ctrl+S saves, Ctrl+Z / Ctrl+Y undo
and redo, Ctrl+D duplicates the selection. A shader that stops compiling keeps its last
working version. One that never compiled draws as a magenta checker.

The compiler walks back from the Surface Output, so nodes that don't reach it cost nothing.
Identical expressions are computed once, and subgraphs are inlined. A `float` converts to any
width, a wider value converts to a narrower one, and other mismatches are errors on the node.
`CherryEngineTests` checks these rules and compiles every node type through DXC.
