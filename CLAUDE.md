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
- `Dice/`: a port of a dice roll feel prototype (Three.js original in `C:\Users\gabri\DiceDemo`). Pure simulation in `DiceSim` (struct of arrays), every die drawn through `Renderer::MeshInstances`; the HUD is an RmlUi document (`Content/UI/Hud.rml`, filled by `DiceHud.cpp`); plan, decisions and milestone status in `Dice/Docs/Port.md`.
- `CherryLaunch/`: the launcher executable.

`README.md` covers the renderer passes, the Rubik controls and how to write a game module.

## Building and running

The build uses Visual Studio 2026 (generator "Visual Studio 18 2026") and vcpkg manifest mode (`vcpkg.json`: DXC, Jolt, RmlUi with FreeType).

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

**Android (`Dice/Docs/Android.md`, `CherryEngine/Docs/Vulkan.md`):** `Tools/Android/Build.ps1 [-Config Release] [-Abi arm64-v8a,x86_64] [-Configure]` cross-compiles the engine and Dice (NDK from the Android SDK, the newest installed; vcpkg triplets `arm64-android`, `x64-android-sse4`) into `out/build/android-<abi>/CherryLaunch/<Config>/libgame.so`. Off Windows `CHERRY_EDITOR` is 0: no editor, module host, D3D12 or DXC. Android runs the shared renderer on the Vulkan backend (Vulkan 1.1+, with fallbacks for missing mutable descriptors, dynamic rendering and synchronization2). Phones can't compile shaders: `Tools/Android/Package.ps1 -Project Dice/Dice.cherryproject [-Config Release] -Abi arm64-v8a,x86_64 -Install -Run` cooks them first with the Windows build (`CherryLaunch <project> --cook-shaders <dir>`; it needs `out/build/x64-debug` built) and ships `ShaderCache/` in the APK (`out/android/<Module>-<Config>.apk`; x86_64 is for the emulator AVD `Cherry_API_32_x64`); the Gradle template is `Android/`; the editor's File > Export > Android APK runs the same script. A shader that wasn't cooked logs "This shader wasn't cooked" on the device: export again. Keep Win32 calls in engine code behind `_WIN32`.

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
  - ImGui is compiled into the engine DLL and exported, so modules share its context. nlohmann/json is a public dependency of the engine, so modules can use it too.
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
  - **Layers:** `Renderer/` draws through the RHI interface (`RHI/RHI.h`: device, command list, buffers, textures, pipelines, constant ring, timestamps), with two backends on Windows: D3D12 (`RHI/D3D12/D3D12Device.cpp`) and Vulkan 1.3 (`RHI/Vulkan/VulkanDevice.cpp`, volk + VMA). The same HLSL compiles to DXIL or SPIR-V (`ShaderDesc::spirv`). It's bindless (SM 6.6 `ResourceDescriptorHeap`; on Vulkan one array of mutable descriptors) with two frames in flight and HDR. Pick the API with Edit > Graphics API in the editor (restarts; remembered in `%LOCALAPPDATA%/CherryEngine/GraphicsApi.txt`), `--rhi d3d12|vulkan` or `CHERRY_RHI`; D3D12 is the default, and a failed Vulkan start falls back to it. In shaders, mark `gDraw` with `CHERRY_PUSH_CONSTANTS` and vertex inputs with `CHERRY_LOCATION(n)`. ImGui is drawn by `Renderer/ImGuiRenderer.cpp` (texture id = bindless SRV index + 1). Plan and status: `CherryEngine/Docs/Vulkan.md`.
  - **Passes:** opaque mesh pass (same mesh + surface shader runs are instanced: the VS reads `instances[gDraw.instanceIndex + SV_InstanceID]`), then ray-marched voxel pass (writes `SV_Depth`), then the sky (`Shaders/SkyPass.hlsl`, only where depth is still clear), then translucent meshes, then debug lines, then bloom (`Shaders/Bloom.hlsl`, compute chain), then ACES tonemap. Before all of it, with `RenderSettings::shadows` on, two sun shadow cascades are drawn depth-only (`Shaders/ShadowPass.hlsl`; casters are gathered whether or not the camera sees them), and `ShadeSurface` darkens the sun through `SunShadow` (`LightingUtil.hlsl`), so meshes and voxels both receive them. Voxel volumes cast shadows too (`Shaders/VoxelShadow.hlsl` ray-marches them along the sunlight into the shadow maps). After the tonemap, FXAA (`Shaders/Fxaa.hlsl`, `RenderSettings::antiAliasing`, on by default) smooths edges.
  - **Mesh formats and fading:** `CreateMesh(data, MeshFormat::Compact)` stores vertices in 28 bytes instead of 48 (normal 8-bit, uv and colour half floats; VertexLayout::MeshCompact); each surface shader gets a compact pipeline variant on first use (like the voxel variant), and the shadow pass has `shadow_compact`. `GetMeshBytes` reports a mesh's GPU memory. `MeshRenderer::dither` / `ditherInvert` fade a mesh screen-door style (a 4×4 pattern in `MeshPass.hlsli`, through `InstanceData::ditherCut`); two meshes at t and 1 - t inverted cover every pixel once, for crossfading levels of detail. Shadows aren't dithered.
  - **Entity-free instances, orthographic views:** `Renderer::MeshInstances()` is a per-frame list (mesh, material, matrix, `tint`, castShadows) drawn like MeshRenderer entities (culled, instanced, shadows); `InstanceData::tint` multiplies the vertex colour. `Camera::orthographic` / `orthoHalfHeight` give an orthographic view (rays from the near plane, box culling, ortho cascade fit). `ShadowSettings::boundsExtents` (non-zero) fits the shadows to a fixed world box instead of the view. `RenderSettings::toneCurve = false` skips the ACES curve (colours are only clipped).
  - **Resources:** assets are handle-based pools, e.g. `CreateVoxelVolume`, `UpdateVoxelVolume(handle, offset, size, data)` and `DestroyVoxelVolume`.
  - **Uploads:** `rhi::Buffer::Upload`, `Texture::Upload` and `Texture::UploadRegion` (so `CreateMesh`, voxel and light volume updates) go through `Device::ExecuteAsync`: submitted on the direct queue without waiting, so the copy runs before any later frame, and the staging buffer is released once the GPU is done. `ExecuteImmediate` still waits (DDS loading). Before this, every runtime `CreateMesh` stalled about 6 ms waiting for the frame in flight.
  - **Light volumes, clouds, SSAO:**
    - `Renderer::CreateLightVolume` makes an R8 3D texture; `InteriorSettings::lightVolume` stretches one over the interior box. Inside, the world's ambient × level × `lightStrength` is added to the interior's, and the sun reaches in where level × `sunFromLight` ≥ 1 (its shadows decide exactly where).
    - `RenderSettings::clouds`: noise on a layer at `height`, drifting with `wind`. It shades the sun (`CloudShadow` in `LightingUtil.hlsl`) and is drawn in `SkyPass.hlsl`.
    - `RenderSettings::heightFog`: exponential height fog replacing the linear fog outside the interior (`HeightFogAmount`/`HeightFogColor` in `LightingUtil.hlsl`, also on the sky's horizon). Particles can `stretch` along their velocity (streaks: rain, sparks).
    - `RenderSettings::edgeHighlights`: `Shaders/Edges.hlsl` finds convex block edges from the depth (after SSAO); the tonemap brightens them.
    - `RenderSettings::colorGrade` (white balance, saturation, contrast, lift, gain, vignette) and `RenderSettings::eyeAdaptation` (`Shaders/EyeAdaptation.hlsl`: a GPU-only exposure that follows the view's brightness, kept in a 1x1 texture) are applied by the tonemap. UNDERTIME sets a grade per era (`cfg::kEraGrades`) and a cave grade.
    - `RenderSettings::lightShafts`: `Shaders/LightShafts.hlsl` marches the view rays at half size through the sun's shadow maps, clouds and (inside) the light volume, after SSAO; the tonemap adds it to the world like the AO.
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
- **Game UI (`UI/Ui.h`, `Renderer/UiRenderer.cpp`, `Shaders/Ui.hlsl`):** RmlUi 6.2 (vcpkg `rmlui`, HTML/CSS-like `.rml` documents styled by `.rcss`, data models bind C++ values). `engine.GetUi()`; RmlUi's API is public to modules (`<RmlUi/Core.h>`; on Windows `rmlui.dll` is shared).
  - **Session:** one context per play session, made before `OnStartup` and removed after `OnShutdown` with every document and data model, so nothing outlives a hot-reloaded DLL. Don't register RmlUi factories from a module (they're global).
  - **Files:** paths are under Content (`ui.LoadDocument("UI/Hud.rml")`; a leading `/` in a document is Content's root). Saving an `.rcss` restyles open documents in place; saving an `.rml` reloads that document (its `ElementDocument*` changes: keep state in data models).
  - **Drawing:** after the tonemap and FXAA, into the LDR view (`Renderer::RenderUi`), so it shows in the editor viewport too. Geometry is streamed each frame into upload buffers (RmlUi keeps compiled vertices alive), transforms and effect parameters go through a per-frame `StructuredBuffer<float4>`, blending is premultiplied (`GraphicsPipelineDesc::premultipliedBlend`, `VertexLayout::RmlUi`).
  - **Effects:** all of RmlUi 6's render interface, after its OpenGL 3 reference renderer: clipping of rounded and transformed boxes (an R16F count texture standing in for a stencil buffer, tested by every UI shader), layers (layer 0 is the view, so `backdrop-filter` blurs the game), `filter`/`backdrop-filter` (opacity, blur with a chain of half-size textures, drop-shadow, brightness, contrast, invert, grayscale, sepia, hue-rotate, saturate), `mask-image`, `box-shadow`, and linear/radial/conic gradients (repeating too) as `decorator`s. RmlUi clips a box-shadow's shape to outside the box before blurring it, so an unoffset blurred box-shadow is faint: a glow is `filter: drop-shadow(colour 0 0 blur)`. Effect targets are view-sized textures made on first use. Dice's `DICE_DEV_GALLERY=1` (`Screenshot.ps1 -Gallery`) shows a tile of each effect (`Dice/Content/UI/Gallery.rml`, also parsed by `DiceTests`). Cost: the gallery's 15 effects are 85 render-target switches, 0.13 ms GPU on desktop; on phones (tile-based GPUs) each switch costs more.
  - **Input:** `Ui::ProcessInput` runs before the game's update. While the mouse is over an element the game's `Input` reports no mouse (`Input::SuppressMouse`); a full-screen body needs `pointer-events: none`. No keyboard or touch yet. F11 toggles RmlUi's debugger while playing. `cursor: pointer` shows a hand.
  - **The editor's UI panel (`Editor/UiInspector.cpp`, docked with the Inspector):** while playing, the game's documents as a tree; **Pick** then click in the game view to select an element (Esc cancels; the game gets no mouse meanwhile); the selection's address, border box, classes (remove / add) and properties, each with the style sheet and line it comes from (click: opens it, in VS Code at the line when `code` is installed). Values typed there apply at once as inline styles, which a reload drops: to keep one, edit the .rcss (it hot-reloads). "Copy as RCSS" puts the element's properties on the clipboard as a rule. The selected element is outlined in cyan in the viewport, the one under the mouse in orange. Dev switches: `CHERRY_DEV_EDITOR=1` shows the editor in a game session, `CHERRY_DEV_UI_SELECT="selector"` selects an element and floats the panel (Dice's `Screenshot.ps1 -Editor -UiSelect "#hud" -Size "2400 1300"`).
  - **Performance:** data bindings suit values that change now and then. Something moved every frame in numbers (Dice's 140 score popups) is far cheaper set from code: `element->SetProperty(Rml::PropertyId::Transform, Rml::Transform::MakeProperty({...}))` rather than a bound transform string RmlUi has to parse (Dice: 1.7-2 ms -> about 0.4 ms a frame). `Ui::LastUpdateMs()` times RmlUi's update.
  - **RCSS gotchas:** no user-agent sheet (`div { display: block; }`), `rgba()` alpha is 0..255 or a percentage (0.78 is transparent), borders are `width colour`, `text-shadow` is `font-effect: glow(...)`. Stats: the editor's GPU line ("game ui": draws, passes, vertices, CPU) and `RenderStats::gameUi*`.
- **Physics:** `Physics/PhysicsWorld.h` wraps Jolt, whose types never leak out of the engine. Bodies are integer `BodyId`s.
  - **What the API offers:**
    - static meshes and boxes;
    - dynamic box compounds;
    - raycasts, impulses and velocity queries;
    - a `PhysicsCharacter` (Jolt `CharacterVirtual`).
  - **Layers:** static, moving, debris (`ignoreCharacters = true` on `CreateDynamicBoxes` or `CreateStaticMesh`: collides with everything except characters; `SetIgnoreCharacters` switches a body later) and character.
  - **Limits:** 16,384 bodies, 65,536 body pairs and contact constraints. Past the contact limits Jolt drops contacts and small bodies fall through floors.
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
- **The showcase cave (for checking visuals):** `Content/Scenes/TestCave.scene` (Mine.scene with `CaveWorld.showcase` on, seed `cfg::kShowcaseSeed`), or `UNDERTIME_DEV_SHOWCASE=1` with any scene. `BuildShowcase` (CaveGenerator) carves a hall just inside the entrance (x 12..36, floor y 1, z 6..28): a panel of each ore, the Time crystal and the strange watch on the east wall; stone and gold cracked lightly and badly on the west wall (`VoxelWorld::SetDamage`); a crystal cluster on the floor; the zone-1 shaft's light; a lamp. The log prints a `UNDERTIME_DEV_VIEW` line per camera spot (hall, ores, cracks, crystal, shaft, lamp). Use it instead of hunting through random caves.
- **Cave look:** the scene's CaveWorld uses `Materials/CaveRock.mat` (`Content/Shaders/CaveRock.hlsl`: ore as metal seams in the rock (colours and amounts from `Config.h`, filled into the material's params by `NewWorld` when the .mat has none), crystal glow, pixel-art cracks that continue across blocks, damp rock, shading-only relief: chipped facets and crack grooves that fade out by `kReliefFadeDistance`); it reads the palette layout from `ShaderGlobal(3)` (`VoxelWorld::PaletteLayout`). `CreateCaveLife` hangs lamps in zone 1, lights Time crystal clusters (`CountCrystals`, kept current by `ApplyWorldChange`) and drifts motes around the lantern. Sparks and the cave-in rumble are in `Sparks` and `ApplyWorldChange`.
- **Weather:** `WeatherAt` (DayNight.h); `UpdateLighting` greys the sky, weakens the sun, thickens clouds and haze, rains (an emitter following you outside) and sets `ShaderGlobal(0).z` (wet ground) / `.w` (rain) for `Content/Shaders/Ground.hlsl` (the terrain's `Materials/Ground.mat`).
- **Collision:** `WorldCollision` keeps static Jolt meshes in 8 m chunks. Edits call `MarkDirty`, and `Flush` rebuilds each dirty chunk once per frame at the end of `OnUpdate`.
- **Structure (`Structure.cpp`):** `DetachUnsupported` flood-fills from an edit to find rock no longer connected to the anchored shell. That rock is cut out of the grid as `FragmentVoxels`.
- **Boulders (`Fragments.cpp`):** rock that broke off in a cave-in is a physics body plus its own small voxel volume. The pick breaks it up with `Chip`, which re-splits it into connected pieces. Boulders never bake back into the grid; past `kMaxBoulders` the oldest comes apart into loose blocks.
- **Loose blocks (`LooseBlocks.cpp`):** every voxel the pick removes becomes one 0.25 m cube. `MirrorOutOfFace` places them in the air in front of the face. They never go back into the grid, and none is ever lost:
  - **Live** while it moves: its own body and entity.
  - **Settled** after staying put (within `kBlockStillDrift`/`kBlockStillTurn`, not Jolt's sleep, which a heap being mined rarely reaches) for `kPileSettleTime`: it waits in its pile's queue, still live, and joins at the pile's next rebuild (`kPileBatchTime` after the first waits; `kPileBuildBudgetMs` a frame). Then it's a record in the pile of its 1 m cell (`kPileCell`). Each pile is one merged mesh and one static collision mesh. The Game panel shows rebuild times split into mesh and collision.
  - **Waking:** `ApplyWorldChange` calls `Wake`, which turns settled blocks near the edit live again, plus everything resting on them.
  - **Limits:** more than `kMaxLiveBlocks` live blocks and the stillest settle early. A block that falls out of the world is put back at the mine mouth.
  - **The shovel** takes live and settled blocks (`LooseBlocks::Ref`) into the backpack.
  - **Test flags** (`LooseBlocks::Options`, defaults in `Config.h`, switched in the editor's Game panel under "Loose block tests", kept across hot reload): the old cap with crumbling, piles on or off, live blocks solid to the player, piles solid to the player. The panel also drops 2,000 or 10,000 test blocks.
- **The valley (`Valley.h`, `TerrainMesh.h`, built in `UndertimeModule::CreateScenery`):**
  - **Layout:** from Design.md's map, with numbers in `Config.h` (valley section). `Valley` gives heights, the river (water level never rises downstream, so it steps down in falls), the lake, ground types and tree spots. The mountain over the cave grid starts at the grid's top.
  - **Terrain and LOD:** everything is 0.25 m blocks, drawn coarser far away. 16 m chunks are drawn in regions of 4×4 (one mesh each for ground, water and ground cover), each at 0.25, 0.5 or 1 m blocks by distance from the camera (`cfg::kLodDistance`, with hysteresis), rebuilt on `ValleyScenery`'s worker threads and swapped in as the camera moves (`ValleyScenery::Update`, called every frame with the eye). Chunks the river runs through (`Valley::RiverChunk`) stay 0.25 m at any distance; skirts hide cracks along region edges and between sizes. Collision is 0.25 m, streamed in 16 m chunks around the player (`kCollisionRadius`; a missing chunk under you is built at once), and includes tree trunks and outcrops. `Valley::Detailed` now only decides where the valley floor is (dense trees, small outcrops). Switching levels crossfades (`MeshRenderer::dither`, `kLodFade`); region meshes use compact vertices (`kLodCompact`). The Game panel's "Valley detail" switches all of it while playing (distance presets, including 0.25 m everywhere; crossfade; compact) and shows triangles, mesh memory and collision stats. The editor preview has no camera to follow: it's built once around the town.
  - **Trees** are placeholder `.vox` models in `Content/Models/Placeholders/`, written by the `UndertimePlaceholders` tool (`Undertime/Tools/Placeholders.cpp`); rerun it after changing a generator. Trees have trunk collision. Far trees and crags use a halved version of their model (0.5 m blocks, `kModelLodDistance`).
  - **Scenery** (`ValleyScenery`): terrain, water and trees as entities without EntityInfo, under one root (one folded row in the hierarchy). Built by the module on Play (with collision), and by `ValleyPreviewSystem` in the editor while the scene has a `ValleyWorld` (no collision; its entities are tagged `ValleyPreview` and dropped from the play world in `OnPlayBegin`).
  - **The town is authored in `Content/Scenes/Mine.scene`** as entities with a `.vox` `MeshRenderer` and a `ValleyProp` (`ValleyProps.h`): `eraFrom`/`eraTo` (1 Camp to 6 Gone), `snapToGround` with `groundOffset`, `collide` (boxes baked from the model; exact for quarter turns). Lights and spinning parts can be ValleyProps too. `ValleyWorld.previewEra` picks which era the editor shows and play starts in; **N** cycles the era while playing (debug, until the clock exists).
  - **Strata:** steep ground above the floor is shaped into layers (`kStrataHeight`, risers and near-flat ledges) in `Valley::BaseHeight`. The terrain picks rock or grass by slope, so ledges come out grassy. `BuildTerrainMesh` bands tall rock walls in three shades per layer, and the cave face (`Cliff`, `Cliff2`, `Cliff3` blocks) uses the same layers.
  - **Outcrops:** `Valley::Outcrops` gives crags on steep ground. `ValleyScenery` places the `Outcrop*.vox` placeholders on the valley floor and `Crag*.vox` (four times the size, in 0.25 m blocks) on the mountains.
  - **Trails** are `cfg::kPathSegments` (trodden ground, no trees).
  - **Water:** the water mesh carries foam hints: a top face's red = 1 - churn (near a fall), a fall face's green = 1 - drop / 4 m (`BuildWaterMesh`). Chunks the river runs through are always fine blocks (`Valley::Detailed`). `Content/Shaders/Water.hlsl` (translucent but writing depth, `#define CHERRY_SURFACE_DEPTH_WRITE`, so overlapping falls don't flicker; fall faces sit 2 cm out from the riverbed with a lip and overlapping corners) with a flow map (`ValleyScenery::BuildFlowMap`: flow, depth, foam below falls) as the material's texture 0; spray emitters at the falls (`FallSprays`).
  - **Foliage:** trees and ground cover use `Materials/Foliage.mat` (`Content/Shaders/Foliage.hlsl`, swaying with the wind in `ShaderGlobal(2)`). Ground cover (tufts, flowers, bushes, pebbles) is scattered by `ValleyScenery` into the terrain region meshes of 0.25 m regions (so within `kLodDistance[0]`); its vertex alpha carries the sway weight. `Ground::Moss` is the forest floor (`Valley::ForestPatch`).
  - **Houses:** the placeholder generator gives buildings open doorways, furniture (`Furnish`, by `Inside` kind) and a hanging lamp. Windows are glass. Town props use `Materials/Town.mat` and `Materials/TownGlass.mat` (`Shaders/TownGlass.hlsl`: the glass glows at night). `ValleyProps::Rooms` derives each building's room from its windows, and `UpdateLighting` passes the nearest 16 to the renderer.
  - **Building pads:** every ValleyProp with `collide` and `flattenGround` levels the ground under its footprint (`Valley::SetPads`, from `ValleyProps::Pads`, before the terrain is built; a house inside its plot takes the plot's height). Pads cover all eras. In the editor, moving a building needs "Rebuild valley" to re-level the ground. Turn `flattenGround` off for things over water (the footbridge).
  - **Dev screenshots:** `Undertime/Tools/Screenshot.ps1` launches the game at a spot, time, weather or the showcase cave and captures the window to a PNG; how to use it (and good camera spots) is in the project skill `.claude/skills/undertime-screenshots/SKILL.md`. Underneath, the environment variable `UNDERTIME_DEV_VIEW="x y z yaw pitch [hold]"` skips the title and starts there (`hold`: the player stays put, for views in mid-air); `UNDERTIME_DEV_SUN="x y z"` overrides the sun direction (e.g. a low sun to check shadows); `UNDERTIME_DEV_TIME="hour [years]"` starts at that hour, optionally that many years after arriving (and in that era); `UNDERTIME_DEV_WEATHER="overcast rain wet"` fixes the weather (0..1 each); `UNDERTIME_DEV_SEED=N` always builds that mountain (the log prints each shaft's foot, for views in the cave). `UNDERTIME_DEV_LOD="a b c"` overrides the valley's LOD distances (`cfg::kLodDistance`: 0.25 m with ground cover, 0.25 m, 0.5 m; e.g. "110 99999 99999" for 0.25 m everywhere); `UNDERTIME_DEV_VALLEY="fade compact [seconds]"` (0/1 each) switches crossfading and compact vertices; `UNDERTIME_DEV_TALK="id [continues]"` stands you in front of a person and opens a conversation (screenshots of the dialog box); `UNDERTIME_DEV_MISSIONS="ids... [journal|board]"` takes on missions and opens the journal or the notice board. `UNDERTIME_DEV_BLOCKS=0` logs frame, GPU and valley triangle counts every two seconds.
- **People (`Npcs.h`, `NpcTown.h`, `NpcWorld.h`; design in `Undertime/Docs/NPCs.md`):**
  - **The cast** is written by hand in `Npcs.cpp` (no generated NPCs): traits, look, ties, eras and a day of `ScheduleBlock`s (hour, place, activity, rain place). Places are on props by EntityInfo name in model space (keep them in step with `Tools/Placeholders.cpp`), on the ground, or on the river bank.
  - **Where someone is** = `npc::Town::Evaluate(npc, days, era)`: pure, from the outside clock only (no per-frame state), so dives and rewinds just ask again. Walks follow the trail graph (`WaypointGraph`) at `kNpcWalkSpeed` real m/s at the surface rate. `Tests/NpcTests.cpp` checks that every walk ends before the next block; a schedule change that fails it needs longer blocks.
  - **Talking** (`Dialog.h`, `Content/Dialog/*.dialog`, format documented in `Dialog.h`): E opens a `npc::Conversation` (state `Talking`, the clock pauses). Friendship is folded from `npc::PlayerLog` (`NpcLog.h`): talks, choices, gifts, gossip, fading; `TurnBackTo` for rewinds (the ageless keep their memories). `Tests/DialogTests.cpp` loads every dialog file, so a typo fails the tests.
  - **Lives** (`Life.h`): `LifeAt` (born, alive, present, age, role) from the cast's dates and story hooks; `EventsBetween` (arrivals, births, weddings, roles, deaths, funerals, Founders' Day) drive the news, letters (`Letters.h`, "letters" in dialog files, read at the post) and gatherings, which `Town::DayBlocks` splices into people's days (`gather.<fire|square|graves>#slot` places). A person needs a day for the era (`DayFor`: `day` for eraFrom..eraTo, `eraDays` for later eras) as well as being alive. Graves for the dead are spawned by the module (`kGraveRow`).
  - **Tools** (`NpcTools.h`): the People inspector and the Dialog view. `NpcToolsSystem` (edit mode, scenes with a ValleyWorld) draws them as two dockable windows from inside `OnEditUpdate` (systems run inside the ImGui frame), with a town built from the edit scene; while playing they're panels in the Game panel. `npc::Describe(Condition)` writes conditions in words.
  - **Missions** (`Missions.h`, `Content/Missions/*.mission`, format in `Missions.h`; `Board.mission` holds notice-board job templates): offered and finished through conversations (`MissionBook::Topics` via `Conversation::Hooks`, which also carry the backpack effects), states folded from the player log, deadlines in outside days (`Expire`), meet windows (`Arrived`), lost things placed in caverns by the module, the board beside the trading post, the journal on Tab. `Tests/MissionTests.cpp`.
  - **The save game** (`SaveGame.h`): `<project>/Saved/SaveGame.json` (clock, position, backpack, gear, the log; not the mined cave). C on the title screen continues. Development views (`UNDERTIME_DEV_VIEW`, `UNDERTIME_DEV_TALK`) never save.
  - **Puppets** (`NpcWorld`): one `.vox` per part in `Content/Models/Npcs/<id>/` (Head, Torso, Arm, Leg; placeholders written by `UndertimePlaceholders` at `kNpcVoxelSize`), joined from the models' sizes and animated in code. Time-lapse figures while time races use `MeshRenderer::dither` and `castShadows = false`. Play only (not in the editor).
- **The working loop (Docs/Hauling.md; branch `features/hauling`):** UndertimeModule methods split into their own files.
  - **Money, shops and the yard:**
    - `Economy.h` (pure): items (block names or lower-case ids), shops and their offers by era, prices, bench and cooking recipes, town projects.
    - `Hauling.cpp`: the purse, the shop window (a "Let's trade." topic from `ShopTopics`; `npc::Effect::Kind::Shop`), the stone yard (`LooseBlocks` town blocks: paid once in `TakeYardPaid`, used up by town projects through `TakeFromYard`), and the save (`SaveGame::hauling`, a JSON blob).
  - **The wheelbarrow:** `Barrow.cpp`.
  - **Your claim:** `Building.h` (pure): pieces, the grid, support, costs, voxel models. `Claim.cpp`: the hammer, sites, the merged mesh and collision, the tent, crates, the bench, sleeping. Its ground is a pad added in `CreateScenery`.
  - **Food, wood and farming:** `Food.cpp` (hunger), `Wood.cpp` (the axe; `ValleyScenery::SetTreeGrowth` hides felled trees and rebuilds their trunk collision), `Farm.cpp`.
  - **Growth:** `Growth.h` (pure): when trees and crops are grown, from the day and the clock.
  - **Placed things and machines:** `Placeables.cpp` (props hold rock up through `DetachUnsupported`'s `held`; ladders; chutes; torches) and `Machines.cpp` (drills, belts, the steam engine, the stamp mill, rails, the hand cart).
  - **Own time:** placed things work at `OwnRate(where)`: your outside rate over the rate there.
  - **Models:** code-made ones come from `ModelKit.h`.
  - **Keys:**
    - 3: the hammer (right mouse: pieces; R: turn; X held: take down).
    - 4: the axe.
    - F: eat.
    - E: use (barrow, cart, furniture, doors, plots, machines).
    - B: what the shovel fills.
    - R: the pack's stone into the barrow.
    - Q: tip the barrow or cart.
  - **Development:** `UNDERTIME_DEV_HAUL="words"` (`Screenshot.ps1 -Haul`; the words are at the bottom of `Hauling.cpp`, e.g. `claim house`, `barrow 120 tip`, `place steam_drill x y z dx dz fast 3`).
- **The outside clock (`Clock.h`, `DayNight.h`, both pure and tested in `Tests/ClockTests.cpp`):**
  - **Clock and rate:** `OutsideClock` counts outside days since you arrived. `UndertimeModule::UpdateClock` advances it at `OutsideClock::Rate` (the surface rate outside, rising through the entrance passage, then `cfg::kZoneRate` per zone).
  - **Eras and the summary:** the era follows the clock, but only changes while the valley can't be seen (deep in, or inside and facing away from the mouth). Coming out shows how much time passed.
  - **Shadow steps:** `UpdateLighting` points the shadows (`ShadowSettings::direction`) at the sun in `kShadowStepDegrees` steps, so shadow edges don't shimmer as time passes; the sun itself moves smoothly. With `kShadowFadeSeconds` > 0 the engine keeps a second set of shadow maps and fades the old shadows into the new after each step (`ShadowSettings::fadeSeconds`); **J** toggles it while playing.
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
    - **] / [:** outside time 10 times faster / slower (up to x1000).
    - **J:** shadows fade between steps, or jump.
    - **Tab:** the journal (missions); **E:** talk, read the notice board, pick up a lost thing.
    - **G:** fly through everything (Space up, Ctrl down); **Shift:** move `kDebugRunMultiplier` times faster, walking or flying.
    - **M:** cave map.
    - **V:** switch between ray-marched and mesh rendering.

## UNDERTIME status and design

- **Design documents** (in `Undertime/Docs/`):
  - `Design.md`: decisions and why: the time rule, rewind rules, the loop and progression, eras, story, the valley and town, the visual style, the plan, playtest questions. Items are marked Decided, Proposed or Open. Read it before UNDERTIME feature work, and update it when the user changes a decision.
  - `Status.md`: what's built, what the user has tested, branches and commits, what's next.
  - `Changelog.md`: how each milestone was built, measurements, fixes after the user's looks. Add to it when a milestone is done, instead of writing build history into Design.md.
  - `NPCs.md` and `Hauling.md`: those systems' plans and status.
- **Current work:** branch `features/hauling` (milestones 1–15, NPC milestones 1–5 and hauling H1–H12 done; most untested by the user; see `Status.md`). The biggest gaps: the mined cave isn't saved, rewinds, the watches and the ending (Design.md §3, §5).
- **User testing:** milestones 2–7 were tested by the user (2026-11-21).
- **Commits:** the user commits when they ask. The list is in `Status.md`.
- **Style:** everything is 0.25 m voxels. The cave is real (diggable) voxels; everything outside is voxel models baked to meshes. Placeholder art is generated as `.vox` files; MagicaVoxel is the editor for now, and a built-in voxel designer may replace it later.

## Working rules for UNDERTIME (from the user)

- **Never invent engine APIs.** Only call functions that exist. If a feature is missing, say so and propose the smallest engine addition or a workaround in game code. Wait for the user's OK before changing anything under `CherryEngine/`.
  - **Except:** engine additions the agreed visual style needs (Design.md §8, "Engine additions": sky, global shader inputs, lights, instancing, shadows, bloom and similar) are pre-approved, and so is the `.vox` voxel model pipeline. Name each engine change in the plan and the report.
- **Work in milestones.** Each one must build and run. Then stop and report what to test and what couldn't be verified.
- **Tuning numbers** all live in one place: `Config.h`.
- **Audio** is out of scope for now.
- **Branches:** feature work happens on separate `features/*` branches. Commit only when asked.
