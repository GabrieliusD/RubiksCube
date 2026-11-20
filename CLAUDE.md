# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

CherryEngine: a small DirectX 12 engine (C++17, Windows only) plus game modules built on it, split like Unreal:

```
CherryLaunch.exe ──loads──▶ <Game>.dll ──links──▶ CherryEngine.dll
```

- `CherryEngine/`: the engine (DLL).
- `Rubik/`: the Rubik's cube game. It's the default module.
- `Undertime/`: UNDERTIME, a first-person voxel mining game. Most active work happens here.
- `CherryLaunch/`: the launcher executable.

`README.md` covers the renderer passes, the Rubik controls and how to write a game module.

## Building and running

The build uses Visual Studio 2026 (generator "Visual Studio 18 2026") and vcpkg manifest mode (`vcpkg.json`: DXC, Jolt).

- **Which `cmake`:** the `cmake` on PATH may not know the VS 2026 generator. Use the copy that ships with Visual Studio: `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`. `ctest.exe` is in the same folder.
- **Build directories:**
  - `out/build/x64-debug`: the modular build, preset `x64-debug`. One multi-config tree holds both Debug and Release.
  - `out/build/x64-monolithic`: configured with `-DCHERRY_MONOLITHIC=ON`, which links the engine and game into a single exe. Its `CHERRY_ACTIVE_GAME_TARGET` is still `RubikGame`, so that exe runs the Rubik game, not UNDERTIME.

```bash
cmake --preset x64-debug                                   # configure (once)
cmake --build out/build/x64-debug --config Debug           # or --config Release
cmake --build out/build/x64-debug --config Debug --target UndertimeGame   # just the game DLL
cmake --build out/build/x64-monolithic --config Release    # check the monolithic build still links
```

Every executable and DLL goes to `out/build/<preset>/bin/<Config>/`. Each build copies shaders, the DXC/Jolt DLLs and the games' `Content/` folders there.

**Run:**
- **Projects:** each game is a project, a `.cherryproject` file next to its CMakeLists (`Undertime/Undertime.cherryproject`, `Rubik/Rubik.cherryproject`). Its Content/ is read from the source tree.
- `bin/<Config>/CherryLaunch.exe Undertime/Undertime.cherryproject` opens the project in the editor. Add `--play` to start playing at once.
- `bin/<Config>/CherryLaunch.exe UndertimeGame.dll` still works: it opens that module's project and plays.
- With no argument, the launcher reopens the last project, or shows the project picker.
- **Editor play mode:** F5 plays (`OnStartup` in a fresh play world) and stops (`OnShutdown`, then the world is destroyed). After Stop the log reports GPU descriptors in use, so a game that leaks resources shows up.

**Game DLLs hot-reload.** The editor loads a copy of the game DLL from `<project>/Saved/Modules/`, so building `UndertimeGame` while the game runs works, and the new DLL is reloaded automatically (Stop, reload, Play). **`CherryEngine.dll` is still locked** by a running editor: anything that relinks the engine (engine code, CMake changes) fails with LNK1168/LNK1104 until the game is closed. If the user has the Release game open, build and test with the Debug configuration instead.

**Tests:** these are plain executables registered with CTest. Each one prints a `[pass]`/`[FAIL]` line per check and exits non-zero on any failure.

```bash
ctest --test-dir out/build/x64-debug -C Release           # all (UndertimeTests ~15 s Release, ~25 s Debug)
ctest --test-dir out/build/x64-debug -C Debug -R UndertimeTests   # one suite
out/build/x64-debug/bin/Debug/UndertimeTests.exe          # run directly to see each check
```

- **Suites:** `CherryEngineTests` covers scenes, hierarchy, reflection, scene files (save/load round trip, unknown components, play-world copy) and shader graphs (`Tests/ShaderGraphTests.cpp`: type rules, parameter layout, subgraphs, and every node type compiled through DXC with `CompileSurfaceShaderOffline`). `RubikPuzzleTests` covers the puzzle logic. `UndertimeTests` covers voxel helpers, fragments, the structure/support check, cave generation over 20 seeds, and the hits needed to crack rock.
- **Running one check:** there is no per-test filter. To focus on one check, edit or comment out code in the suite's `main()`.
- **What the tests can't reach:** they link only the pure-C++ sources (see `Undertime/CMakeLists.txt`), so anything that needs `Engine` can't be tested there.

**Shaders:**
- **Hot reload:** HLSL (SM 6.6) is compiled at runtime by DXC. Debug builds read `CherryEngine/Shaders` straight from the source tree and hot-reload it on save.
- **Shared layouts:** `Shaders/Shared/ShaderInterop.h` is included by both the C++ code and the HLSL.

The `.github/` folder exists but is empty: there is no CI.

## Engine architecture (big picture)

- **Frame loop (`Core/Engine.cpp`):**
  1. The editor draws first and decides who owns input.
  2. `game.OnUpdate` runs.
  3. Fixed-step physics runs.
  4. Every entity with a `RigidBody` gets its `Transform` overwritten with the interpolated physics pose.
  5. Render, then present.

  Because of step 4, game code can't move a rigid-body entity by writing its `Transform`. Only fields the physics sync leaves alone stick, such as `VoxelVolumeRenderer::voxelSize`.
- **Game module interface:**
  - `IGameModule` (`Game/GameModule.h`) has `GetConfig`, `RegisterTypes`, `OnStartup`, `OnUpdate`, `OnImGui`, `OnShutdown`, and `SaveState`/`LoadState` for hot reload during play. `ISystem`s registered by the game run alongside it (and optionally in edit mode).
  - `CHERRY_IMPLEMENT_GAME_MODULE` exports `CreateGameModule`.
  - A game's CMake calls `cherry_register_game_module(<target>)`, which builds it as a SHARED library, or as STATIC in the monolithic build.
  - ImGui is compiled into the engine DLL and exported, so modules share its context.
- **ECS and scene:** `Scene/` holds the ECS (65,536 entities, 64 component types). The built-in components are `Transform`, `MeshRenderer`, `Camera`, `VoxelVolumeRenderer`, `RigidBody`, plus `EntityInfo` (guid + name), `Parent`, `PointLightComponent`, `Environment` and `UnknownComponents`.
  - **Reflection (`Scene/Reflection.h`):** `TypeRegistry` holds a field list per component. It drives scene files, the play-world copy and the inspector. Games register theirs in `IGameModule::RegisterTypes`.
  - **Scene files (`Scene/SceneSerializer.h`):** JSON. Only entities with an `EntityInfo` are saved; entities made by code at runtime have none.
  - **Hierarchy:** `Transform` is local to the `Parent`; use `Scene::WorldMatrix`/`WorldPosition` for world space. Rigid bodies ignore parents.
- **Scripts (`Game/Script.h`):** a `Script` subclass is a component with behaviour, registered with `types.Script<T>()` (inherit fields with `.Base<B>()`). The engine runs `OnStart`/`OnUpdate`/`OnDestroy` while playing, after the game's `OnUpdate`. Component arrays are `std::deque`, so a script's `this` survives spawning; use `Destroy()`/`Engine::DestroyLater` (end of frame), not `DestroyEntity`, for entities that have scripts. Built-ins: `Spinner`, `Spawner`, `Lifetime`.
  - **Self-registration:** `CHERRY_SCRIPT(Type, "Name") { script.Field(...); }` in a script's `.cpp` (needs `Game/GameModule.h`) registers it without touching `RegisterTypes`. Each game's `Scripts/` folder is globbed into its target (`CONFIGURE_DEPENDS`). The editor's New Script writes there and compiles with a configure step first, because a VS-generator build that regenerates projects only compiles new files on the next build. Monolithic builds link the game with `/WHOLEARCHIVE` so unreferenced self-registering objects aren't dropped.
- **Prefabs:** `.prefab` files (scene JSON for one entity tree). `Engine::Spawn(path, position)` places one; `cherry::SavePrefab`/`InstantiatePrefab` in `Scene/SceneSerializer.h`. Instances are copies (`EntityInfo::prefab` remembers the source).
- **Editor worlds:** the editor edits the *edit world*. Play copies it (`cherry::CopyScene`) into a *play world*, runs `OnStartup` there and throws it away on Stop. `engine.GetScene()` is the active one.
- **Assets (`Core/Assets.h`):** referenced by path under the project's `Content/` (`Meshes/x.glb`, `Materials/x.mat`, `Textures/x.png`, built-ins `engine/Cube`). `MeshRenderer.meshAsset`/`materialAsset` are resolved to handles by the engine; game code that makes meshes sets the handles directly and leaves the asset paths empty. Files reload when they change on disk.
- **Renderer:**
  - **Layers:** `Renderer/` sits on the thin D3D12 layer in `RHI/`. It's bindless (SM 6.6 `ResourceDescriptorHeap`) with two frames in flight and HDR.
  - **Passes:** mesh pass, then ray-marched voxel pass (writes `SV_Depth`), then ACES tonemap, then debug lines.
  - **Resources:** assets are handle-based pools, e.g. `CreateVoxelVolume`, `UpdateVoxelVolume(handle, offset, size, data)` and `DestroyVoxelVolume`.
  - **Lighting:** GGX PBR (metallic/roughness) in `Shaders/LightingUtil.hlsl` (`ShadeSurface`), shared by the mesh and voxel passes. A light's `Strength` includes pi, so Lambert is `albedo * Strength * NdotL`.
  - **Surface shaders:** HLSL defining `SurfaceOutput EvaluateSurface(SurfaceInput)` (`Shaders/Surface.hlsli`), wrapped around `MeshPass.hlsli` into a pipeline of its own (`Renderer::CreateSurfaceShader`). `Mesh.hlsl` is the built-in one. A material picks one with `MaterialDesc::shader` and passes values through `params`/`textures`, a per-material parameter block. `.hlsl` files in Content are "shader" assets that a `.mat` references (`"shader"`, `"params"`). A shader that fails to compile keeps its last good version, or shows a magenta checker if it never compiled. Surface shader pipelines build on worker threads (`PipelineLibrary::SetGraphicsAsync`, applied in `PumpAsync` at `BeginFrame`): `CreateSurfaceShader`/`SetSurfaceShaderSource` return at once and report through a `done` callback, and a graph's parameter layout switches when its code does. A surface shader can be translucent (blended, drawn after the voxel pass, back to front) and can define `EvaluateVertexOffset`.
  - **Shader graphs (`ShaderGraph/`):**
    - `ShaderGraph.h` is the graph model and its `.shadergraph` JSON format.
    - `NodeLibrary.h/.cpp` defines every node type: pins, settings and the HLSL it emits.
    - `GraphCompiler.h/.cpp` turns a graph into a surface shader plus a parameter layout (sorted by name).
    - `AssetManager` compiles graphs used as shaders, applies `.mat` `"parameters"` overrides, and reloads dependents when a subgraph changes. `PreviewShaderGraph` serves live editing.
    - The editor is `Editor/ShaderGraphEditor.cpp`, built on imgui-node-editor (vendored in `thirdparty/imgui-node-editor`). Its preview uses `Renderer::RenderPreview`.
    - Generated HLSL is also written to `<project>/Saved/ShaderCache/`.
  - **Voxel palettes:** volumes use 8-bit palette indices. A volume can share another volume's palette (`sharePaletteFrom`).
- **Physics:** `Physics/PhysicsWorld.h` wraps Jolt, whose types never leak out of the engine. Bodies are integer `BodyId`s.
  - **What the API offers:**
    - static meshes and boxes;
    - dynamic box compounds;
    - raycasts, impulses and velocity queries;
    - a `PhysicsCharacter` (Jolt `CharacterVirtual`).
  - **Layers:** static, moving, debris (`CreateDynamicBoxes(..., ignoreCharacters = true)`: collides with everything except characters) and character.
  - **Gotcha:** the player character has an inner body (character layer). A dynamic-only raycast that starts inside the player hits the player first.
- **Legacy files:** `.cpp`/`.h` files sitting directly in `CherryEngine/` that aren't in `CherryEngine/CMakeLists.txt` are leftovers from an older engine and aren't compiled. Examples: `D3DApp`, `RenderSystem`, `Cube` and `CameraSystem`. Ignore them, and don't delete them without asking.

## UNDERTIME architecture

The game is first-person mining in a procedurally generated mountain. One world voxel is 0.25 m (`cfg::kVoxelSize`), and the world grid is 48×24×160 m.

- **Config:** `Config.h` (namespace `cfg`) holds every tuning number, grouped by section, plus the block table (`cfg::Block`, `cfg::Info`). New tunables go there too.
- **Voxel world (`VoxelWorld`):**
  - It's the CPU copy of the grid. It handles raycasts, `Carve` (the pick's damage model with cracking state in `mDamage`), palette building and greedy collision-face extraction.
  - The GPU copy is one ray-marched volume. Changes are uploaded as sub-boxes through `PaletteVoxels(box)` and `UpdateVoxelVolume`.
- **Cave generation:**
  - `CaveGenerator` plus `Noise.h` build the world for a seed: 5 depth zones separated by sealed rock bands, rooms, worm tunnels, noise caverns, sealed pockets, and ore placed by depth fraction.
  - It's deterministic per seed, and `CaveLayout` describes the result for tests and the debug map (`CaveMap`).
- **Collision:** `WorldCollision` keeps static Jolt meshes in 8 m chunks. Edits call `MarkDirty`, and `Flush` rebuilds each dirty chunk once per frame at the end of `OnUpdate`.
- **Structure (`Structure.cpp`):** `DetachUnsupported` flood-fills from an edit to find rock no longer connected to the anchored shell. That rock is cut out of the grid as `FragmentVoxels`.
- **Boulders (`Fragments.cpp`):** rock that broke off in a cave-in is a physics body plus its own small voxel volume. The pick breaks it up with `Chip`, which re-splits it into connected pieces. Boulders never bake back into the grid; past `kMaxBoulders` the oldest comes apart into loose blocks.
- **Loose blocks (`LooseBlocks.cpp`):** every voxel the pick removes becomes one 0.25 m cube on the debris layer (passes through the player), drawn with shared vertex-coloured cube meshes. `MirrorOutOfFace` places them in the air in front of the face. They never go back into the grid; the shovel scoops them into the backpack, and past `kMaxLooseBlocks` the oldest crumble away.
- **How every grid edit is applied:** `UndertimeModule::ApplyWorldChange(box, checkSupport)` uploads to the GPU, marks collision dirty, rebuilds meshes in mesh-render mode, and optionally runs the support check, spawning fragments. Route edits through it.
- **Scene components (`SceneComponents.h`):** `CaveWorld` (seed, 0 = random) and `PlayerStart`, placed in `Content/Scenes/Mine.scene`. `CavePreviewSystem` shows a CaveWorld's mountain in the editor ("Generate preview" in the inspector).
- **Module (`UndertimeModule.cpp`):**
  - **States:** Title, Playing, Paused.
  - **Mining:** swing timing, then `Carve`, then `SpawnBlocksFromFace`. Tools: 1 pick, 2 shovel (`UpdateShovel`), backpack counts per block type.
  - **Feel:** hit feedback (jolt, shake, recoil, impact freeze) and the HUD (`Hud.h`, ImGui draw lists).
  - **Debug keys:**
    - **F6:** drop a test boulder.
    - **F7:** cave-in test.
    - **F8:** new world.
    - **F9:** next zone.
    - **F10:** look down (for scripted tests, which can't move the mouse).
    - **M:** cave map.
    - **V:** switch between ray-marched and mesh rendering.

## Working rules for UNDERTIME (from the user)

- **Never invent engine APIs.** Only call functions that exist. If a feature is missing, say so and propose the smallest engine addition or a workaround in game code. Wait for the user's OK before changing anything under `CherryEngine/`.
- **Work in milestones.** Each one must build and run. Then stop and report what to test and what couldn't be verified.
- **Tuning numbers** all live in one place: `Config.h`.
- **Audio** is out of scope for now.
- **Branches:** feature work happens on separate `features/*` branches. Commit only when asked.
