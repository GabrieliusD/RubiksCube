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

- **Suites:** `CherryEngineTests` covers scenes, hierarchy, reflection, scene files (save/load round trip, unknown components, play-world copy) and shader graphs (`Tests/ShaderGraphTests.cpp`: type rules, parameter layout, subgraphs, and every node type compiled through DXC with `CompileSurfaceShaderOffline`). `RubikPuzzleTests` covers the puzzle logic. `UndertimeTests` covers voxel helpers, fragments, the structure/support check, cave generation over 20 seeds, the hits needed to crack rock, and the valley (`Tests/ValleyTests.cpp`: heights, river, lake, trees, terrain and water meshes). `CherryEngineTests` also has `Tests/VoxelModelTests.cpp` and `Tests/RendererTests.cpp`.
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
- **Assets (`Core/Assets.h`):** referenced by path under the project's `Content/` (`Meshes/x.glb`, `Models/x.vox`, `Materials/x.mat`, `Textures/x.png`, built-ins `engine/Cube`).
- **Voxel models (`Voxel/VoxelModel.h`):** `VoxelModel` data, `LoadVox`/`SaveVox` (MagicaVoxel, Z-up mapped to Y-up), `BakeVoxelMesh` (greedy faces, corner shading in vertex colour, glow strength in `uv.x`, block size in `uv.y` for the shader's per-block brightness) and `BakeVoxelBoxes` (collision). A `.vox` path is a mesh asset, which defaults to the `engine/VoxelModel` material (`Shaders/VoxelModelSurface.hlsli`). Tests are in `Tests/VoxelModelTests.cpp`, with a fixture in `Tests/Data/`. `MeshRenderer.meshAsset`/`materialAsset` are resolved to handles by the engine; game code that makes meshes sets the handles directly and leaves the asset paths empty. Files reload when they change on disk.
- **Renderer:**
  - **Layers:** `Renderer/` sits on the thin D3D12 layer in `RHI/`. It's bindless (SM 6.6 `ResourceDescriptorHeap`) with two frames in flight and HDR.
  - **Passes:** opaque mesh pass (same mesh + surface shader runs are instanced: the VS reads `instances[gDraw.instanceIndex + SV_InstanceID]`), then ray-marched voxel pass (writes `SV_Depth`), then the sky (`Shaders/SkyPass.hlsl`, only where depth is still clear), then translucent meshes, then debug lines, then bloom (`Shaders/Bloom.hlsl`, compute chain), then ACES tonemap. Before all of it, with `RenderSettings::shadows` on, two sun shadow cascades are drawn depth-only (`Shaders/ShadowPass.hlsl`; casters are gathered whether or not the camera sees them), and `ShadeSurface` darkens the sun through `SunShadow` (`LightingUtil.hlsl`), so meshes and voxels both receive them. Voxel volumes cast shadows too (`Shaders/VoxelShadow.hlsl` ray-marches them along the sunlight into the shadow maps). After the tonemap, FXAA (`Shaders/Fxaa.hlsl`, `RenderSettings::antiAliasing`, on by default) smooths edges.
  - **Resources:** assets are handle-based pools, e.g. `CreateVoxelVolume`, `UpdateVoxelVolume(handle, offset, size, data)` and `DestroyVoxelVolume`.
  - **Light volumes, clouds, SSAO:**
    - `Renderer::CreateLightVolume` makes an R8 3D texture; `InteriorSettings::lightVolume` stretches one over the interior box. Inside, the world's ambient × level × `lightStrength` is added to the interior's, and the sun reaches in where level × `sunFromLight` ≥ 1 (its shadows decide exactly where).
    - `RenderSettings::clouds`: noise on a layer at `height`, drifting with `wind`. It shades the sun (`CloudShadow` in `LightingUtil.hlsl`) and is drawn in `SkyPass.hlsl`.
    - `RenderSettings::ssao`: `Shaders/Ssao.hlsl` (`CSMain`, then the depth-aware `CSBlur`) runs before the view models clear the depth. The tonemap applies it only where the final depth is at the far plane, so view models are left alone.
  - **Particles (`Particles/ParticleSystem.h`, `Shaders/Particles.hlsl`):**
    - **Emitters:** `ParticleEmitter` is a scene component (settings, plus its live particles at runtime). It spawns from its entity in play and in the editor. Game code adds one-off bursts with `engine.GetParticles().Burst(settings, position, count, direction)`.
    - **Simulation:** on the CPU. Each frame the engine updates and submits to `Renderer::Particles()`.
    - **Drawing:** billboards drawn after translucent meshes, covering ones back to front, then additive ones (`additive`, e.g. sparks). Each particle is lit in the vertex shader like the scene at its centre (the sun with shadows and clouds, the fills, ambient with the cave interior and rooms, point lights), so dust only shines in sunlight.
    - **UNDERTIME's uses:** smoke and sparks are `ParticleEmitter`s on town props in `Mine.scene` (`ValleyProps::ApplyEra` switches them with the era). Shaft motes come from `CreateShaftDust`; rock dust from `RockDust`, on every bite and cave-in.
  - **Glass and rooms:**
    - A voxel colour can be glass (`VoxelMaterial::glass`, MagicaVoxel's `_glass`). A `.vox` with glass bakes into two meshes, `x.vox` (the opaque part) and `x.vox#glass`.
    - `MeshRenderer` draws the glass part with `glassMaterialAsset`, or the translucent built-in `engine/VoxelGlass` (`Shaders/VoxelGlassSurface.hlsli`).
    - A hand-written `.hlsl` surface shader is translucent if it contains `#define CHERRY_SURFACE_TRANSLUCENT`.
    - `RenderSettings::rooms` holds up to 16 turned boxes: inside one, the sky's ambient drops to `ambientScale` and the fill lights fade out, while the sun still comes in where its shadows let it.
  - **Interiors:** `RenderSettings::interior` is a world-axis box (opening at its min-z face). Inside it, `ShadeSurface` fades out the directional lights and switches to the interior's ambient, pixel by pixel (`InteriorAmount` in `LightingUtil.hlsl`), and `ApplySceneFog` applies the interior's fog only over the part of the view ray that's inside. UNDERTIME sets it to the cave grid, so the valley stays lit when seen from the cave and the tunnel stays dark when seen from outside.
  - **Lighting:** GGX PBR (metallic/roughness) in `Shaders/LightingUtil.hlsl` (`ShadeSurface`), shared by the mesh and voxel passes. A light's `Strength` includes pi, so Lambert is `albedo * Strength * NdotL`. Up to 8 point lights per frame; with more, `SelectPointLights` keeps those closest to reaching the camera. `RenderSettings::shaderGlobals` (4 float4) reach every surface shader as `ShaderGlobal(i)` / the Global Input node.
  - **Surface shaders:** HLSL defining `SurfaceOutput EvaluateSurface(SurfaceInput)` (`Shaders/Surface.hlsli`), wrapped around `MeshPass.hlsli` into a pipeline of its own (`Renderer::CreateSurfaceShader`). `Mesh.hlsl` is the built-in one. A material picks one with `MaterialDesc::shader` and passes values through `params`/`textures`, a per-material parameter block. `.hlsl` files in Content are "shader" assets that a `.mat` references (`"shader"`, `"params"`). A shader that fails to compile keeps its last good version, or shows a magenta checker if it never compiled. Surface shader pipelines build on worker threads (`PipelineLibrary::SetGraphicsAsync`, applied in `PumpAsync` at `BeginFrame`): `CreateSurfaceShader`/`SetSurfaceShaderSource` return at once and report through a `done` callback, and a graph's parameter layout switches when its code does. Voxel volumes with a material use its surface shader too: the ray march is `Shaders/VoxelPass.hlsli` (`VoxelRaymarch.hlsl` is the built-in voxel surface), and the voxel pipeline variant is built on first use (`Impl::VoxelPipeline`). `SurfaceInput` has `positionL` and `blockId` for voxels. A surface shader can be translucent (blended, drawn after the voxel pass, back to front) and can define `EvaluateVertexOffset`.
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
  - **The mountain face:** the grid's front (z = 0) is shaped into craggy ledges with a 1 m skin of unbreakable `Cliff`/`Turf` blocks coloured like the terrain rock and grass (`cfg::Unbreakable`), using a fixed seed so it looks the same every run. The entrance is a rough, arched 3.2 m passage. `Valley::BaseHeight` adds buttresses over the grid's side edges and rubble at its foot. The numbers are in `Config.h` ("The mountain face").
- **Daylight in the cave (`Skylight.h`, tested in `Tests/LightTests.cpp`):**
  - **Spread:** a level per 0.5 m cell, spreading from the face and from openings in the grid's top. Sideways or up costs `kSkylightStep`; down, straight or diagonally, costs `kSkylightDownStep`.
  - **Updates:** `ApplyWorldChange` calls `Skylight::Update`, which only ever adds light (mining only opens rock), and uploads the changed cells to the light volume.
- **Natural shafts (`cfg::kShafts`):**
  - **Where:** fixed points on the grid's top. Each leans north along the noon sun (`kShaftLean`) into a cavern of its zone, and `GenerateCaves` moves the seals so each shaft fits inside its zone.
  - **Rooms and tunnels:** the cavern joins the tunnel network last, as a leaf. Rooms keep clear of the shaft's tube, and tunnels steer around it.
  - **Surface:** `Valley` leaves a hole in the ground over each shaft. `ValleyScenery` puts a log grate (`ShaftGrate.vox`, with collision) at the lowest ground around the hole.
- **Collision:** `WorldCollision` keeps static Jolt meshes in 8 m chunks. Edits call `MarkDirty`, and `Flush` rebuilds each dirty chunk once per frame at the end of `OnUpdate`.
- **Structure (`Structure.cpp`):** `DetachUnsupported` flood-fills from an edit to find rock no longer connected to the anchored shell. That rock is cut out of the grid as `FragmentVoxels`.
- **Boulders (`Fragments.cpp`):** rock that broke off in a cave-in is a physics body plus its own small voxel volume. The pick breaks it up with `Chip`, which re-splits it into connected pieces. Boulders never bake back into the grid; past `kMaxBoulders` the oldest comes apart into loose blocks.
- **Loose blocks (`LooseBlocks.cpp`):** every voxel the pick removes becomes one 0.25 m cube on the debris layer (passes through the player), drawn with shared vertex-coloured cube meshes. `MirrorOutOfFace` places them in the air in front of the face. They never go back into the grid; the shovel scoops them into the backpack, and past `kMaxLooseBlocks` the oldest crumble away.
- **The valley (`Valley.h`, `TerrainMesh.h`, built in `UndertimeModule::CreateScenery`):**
  - **Layout:** from Design.md's map, with numbers in `Config.h` (valley section). `Valley` gives heights, the river (water level never rises downstream, so it steps down in falls), the lake, ground types and tree spots. The mountain over the cave grid starts at the grid's top.
  - **Terrain:** 16 m chunks, using 0.25 m blocks where `Valley::Detailed` and 1 m blocks elsewhere, with skirts where the two meet. Chunks are built on worker threads and merged 4×4 into one mesh and one collision body; remote mountains get 4 m collision.
  - **Trees** are placeholder `.vox` models in `Content/Models/Placeholders/`, written by the `UndertimePlaceholders` tool (`Undertime/Tools/Placeholders.cpp`); rerun it after changing a generator. Trees have trunk collision.
  - **Scenery** (`ValleyScenery`): terrain, water and trees as entities without EntityInfo, under one root (one folded row in the hierarchy). Built by the module on Play (with collision), and by `ValleyPreviewSystem` in the editor while the scene has a `ValleyWorld` (no collision; its entities are tagged `ValleyPreview` and dropped from the play world in `OnPlayBegin`).
  - **The town is authored in `Content/Scenes/Mine.scene`** as entities with a `.vox` `MeshRenderer` and a `ValleyProp` (`ValleyProps.h`): `eraFrom`/`eraTo` (1 Camp to 6 Gone), `snapToGround` with `groundOffset`, `collide` (boxes baked from the model; exact for quarter turns). Lights and spinning parts can be ValleyProps too. `ValleyWorld.previewEra` picks which era the editor shows and play starts in; **N** cycles the era while playing (debug, until the clock exists).
  - **Strata:** steep ground above the floor is shaped into layers (`kStrataHeight`, risers and near-flat ledges) in `Valley::BaseHeight`. The terrain picks rock or grass by slope, so ledges come out grassy. `BuildTerrainMesh` bands tall rock walls in three shades per layer, and the cave face (`Cliff`, `Cliff2`, `Cliff3` blocks) uses the same layers.
  - **Outcrops:** `Valley::Outcrops` gives crags on steep ground. `ValleyScenery` places the `Outcrop*.vox` placeholders: near ones with box collision, far ones baked with 1 m blocks.
  - **Trails** are `cfg::kPathSegments` (trodden ground, no trees).
  - **Houses:** the placeholder generator gives buildings open doorways, furniture (`Furnish`, by `Inside` kind) and a hanging lamp. Windows are glass. Town props use `Materials/Town.mat` and `Materials/TownGlass.mat` (`Shaders/TownGlass.hlsl`: the glass glows at night). `ValleyProps::Rooms` derives each building's room from its windows, and `UpdateLighting` passes the nearest 16 to the renderer.
  - **Building pads:** every ValleyProp with `collide` and `flattenGround` levels the ground under its footprint (`Valley::SetPads`, from `ValleyProps::Pads`, before the terrain is built; a house inside its plot takes the plot's height). Pads cover all eras. In the editor, moving a building needs "Rebuild valley" to re-level the ground. Turn `flattenGround` off for things over water (the footbridge).
  - **Dev screenshots:** the environment variable `UNDERTIME_DEV_VIEW="x y z yaw pitch [hold]"` skips the title and starts there (`hold`: the player stays put, for views in mid-air); `UNDERTIME_DEV_SUN="x y z"` overrides the sun direction (e.g. a low sun to check shadows); `UNDERTIME_DEV_TIME="hour [years]"` starts at that hour, optionally that many years after arriving (and in that era).
- **The outside clock (`Clock.h`, `DayNight.h`, both pure and tested in `Tests/ClockTests.cpp`):**
  - **Clock and rate:** `OutsideClock` counts outside days since you arrived. `UndertimeModule::UpdateClock` advances it at `OutsideClock::Rate` (the surface rate outside, rising through the entrance passage, then `cfg::kZoneRate` per zone).
  - **Eras and the summary:** the era follows the clock, but only changes while the valley can't be seen (deep in, or inside and facing away from the mouth). Coming out shows how much time passed.
  - **Light:** `DayLightAt(hour)` gives sun or moon, sky colours, ambient, stars and `night`. When days pass faster than one per `kStrobeSlowDay` seconds, `UpdateLighting` blends towards `DayLightAverage()` (no strobing).
  - **Shader globals:** `ShaderGlobal(0).x` = night (0..1), `ShaderGlobal(1).rgb` = window light colour. The town's props use `Materials/Town.mat` (`Content/Shaders/TownVoxel.hlsl`): glow strengths under 3 light up only at night, 3 and up always (fire). Prop point lights dim by day (`ValleyProps::SetLightLevel`).
  - **The HUD watch:** `hud::Watch`, bottom right. The big dial shows outside time (cyan hands, a glowing ring when they spin too fast), the small dial your own time. Beside it: the date, the zone and pace, the time since you arrived and an era warning.
- **How every grid edit is applied:** `UndertimeModule::ApplyWorldChange(box, checkSupport)` uploads to the GPU, marks collision dirty, rebuilds meshes in mesh-render mode, and optionally runs the support check, spawning fragments. Route edits through it.
- **Scene components (`SceneComponents.h`):** `CaveWorld` (seed, 0 = random; `rockMaterial`, an optional material for the world volume and boulders, e.g. `Materials/Rock.mat`) and `PlayerStart`, placed in `Content/Scenes/Mine.scene`. `CavePreviewSystem` shows a CaveWorld's mountain in the editor ("Generate preview" in the inspector).
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
    - **N:** jump the clock to the next era (after Gone, back to the arrival day).
    - **T:** skip ahead `kSkipHours` (checking the light through the day).
    - **M:** cave map.
    - **V:** switch between ray-marched and mesh rendering.

## UNDERTIME status and design

- **Design document:** `Undertime/Docs/Design.md` holds every decision so far: the time rule, the run loop, eras, story, the valley and town layout, the visual style and the milestone plan. Read it before UNDERTIME feature work, and update it when the user changes a decision.
- **Built so far:** the cave game (mining, cave-ins, boulders, loose blocks, shovel, backpack) and voxel surface shaders. **Not built yet:** the outside clock, shops, the run reset and the story (the valley, the town and its first two eras exist).
- **Current work** (branch `features/voxel-town`): milestones from Design.md §8, in order:
  1. Voxel model pipeline (`.vox` load and save, mesh and collision baking, a `.vox` asset type). **Done** (the user checked it in the editor and in MagicaVoxel).
  2. Engine visuals I (sky, shader globals, nearest-8 lights, instancing). **Done.**
  3. The valley (terrain, river, lake, forest). **Done.**
  4. Engine visuals II (sun shadows, bloom). **Done.**
  5. Camp era (with the valley shown in the editor). **Done.**
  6. Settlement era. **Done.**
  7. Fixes: voxel shadow casting and anti-aliasing **done**. The loose-block slowdown is **fixed** (big piles never fell asleep: Jolt's sleep threshold is now 0.08 m/s, and `LooseBlocks` puts nearly still blocks to sleep with `PhysicsWorld::Deactivate`; 3k blocks went from 2+ ms of physics to 0.09 ms). `UNDERTIME_DEV_BLOCKS=N` drops N blocks and logs timings.

  8. The outside clock and day/night (time faster underground, HUD watch, eras from the clock). **Done**, not yet tested by the user.
  9. Light and depth: daylight spreading into the cave (light volume), natural shafts, SSAO, cloud shadows. **Done**, not yet tested by the user.
  10. The mountain matches its face (ledges and strata on steep terrain). **Done**, not yet tested by the user.
  11. Houses: interiors, glass windows, indoor light. **Done**, not yet tested by the user.
  12. Particles (engine): dust in the shaft beams, smoke, sparks, mining dust. **Done**, not yet tested by the user.

  Valley life, activities, shops and story come after (ideas in Design.md §9).
- **User testing:** milestones 2–7 were tested by the user (2026-11-21).
- **Commits:** the user commits when they ask. Milestones 1–4 are in `813c198`, 5–6 in the commit after it.
- **Style:** everything is 0.25 m voxels. The cave is real (diggable) voxels; everything outside is voxel models baked to meshes. Placeholder art is generated as `.vox` files; MagicaVoxel is the editor for now, and a built-in voxel designer may replace it later.

## Working rules for UNDERTIME (from the user)

- **Never invent engine APIs.** Only call functions that exist. If a feature is missing, say so and propose the smallest engine addition or a workaround in game code. Wait for the user's OK before changing anything under `CherryEngine/`.
  - **Except:** engine additions the agreed visual style needs (Design.md §8: sky, global shader inputs, lights, instancing, shadows, bloom and similar) are pre-approved, and so is the `.vox` voxel model pipeline. Name each engine change in the plan and the report.
- **Work in milestones.** Each one must build and run. Then stop and report what to test and what couldn't be verified.
- **Tuning numbers** all live in one place: `Config.h`.
- **Audio** is out of scope for now.
- **Branches:** feature work happens on separate `features/*` branches. Commit only when asked.
