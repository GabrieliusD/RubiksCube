# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

CherryEngine: a small DirectX 12 engine (C++17, Windows only) plus game modules built on it, split like Unreal:

```
CherryLaunch.exe ──loads──▶ <Game>.dll ──links──▶ CherryEngine.dll
```

- `CherryEngine/`: the engine (DLL).
- `CherryLaunch/`: the launcher executable.
- `Rubik/`: the Rubik's cube game. It's the default module.
- `Undertime/`: UNDERTIME, a first-person voxel mining game.
- `Dice/`: a port of a dice roll feel prototype, the one game that also runs on Android.

**Each game folder has its own `CLAUDE.md`** (`Undertime/CLAUDE.md`, `Dice/CLAUDE.md`, `Rubik/CLAUDE.md`) with that game's architecture, status and rules. This file covers the engine, the build and how the repository is organised. The engine has no code that depends on a game; keep it that way, so a game folder can later move to a repository of its own.

`README.md` covers the renderer passes, the Rubik controls and how to write a game module.

## Branches and commits

- **`master`** holds the engine and every game. It should always build and pass `ctest`. Everything up to the reorganisation is tagged `v0.1-pre-split`; two superseded branches are kept as the tags `archive/features-ecs` and `archive/features-voxel-graphs`.
- **Work branches are short-lived and named by area:** `engine/<topic>` for engine-only work, `undertime/<topic>`, `dice/<topic>`, `rubik/<topic>` for game work (including the engine additions that game needs). They merge back into `master`; there are no long-lived per-game branches.
- **An engine change is checked against every game:** build all games (the default `CHERRY_GAMES`) and run all the tests before it merges.
- **Commit and push only when the user asks.**

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
cmake -S . -B out/build/x64-debug -DCHERRY_GAMES=Undertime # build only some games ("Undertime;Dice")
cmake -S . -B out/build/x64-debug "-DCHERRY_GAMES=Rubik;Undertime;Dice"   # back to all of them
```

- **`CHERRY_GAMES`** lists the game folders to build (default: all three on Windows, Dice on Android). It's a cache variable, so it stays set until you change it. A monolithic build's `CHERRY_ACTIVE_GAME_TARGET` must be one of them. The editor's project picker still lists every `.cherryproject`; a game that wasn't built has no DLL to load.

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
  - **Drawing:** after the tonemap and FXAA, into the LDR view (`Renderer::RenderUi`), so it shows in the editor viewport too. Geometry is streamed each frame into upload buffers (RmlUi keeps compiled vertices alive), transforms and effect parameters go through a per-frame `StructuredBuffer<float4>`, blending is premultiplied (`GraphicsPipelineDesc::premultipliedBlend`, `VertexLayout::RmlUi`). RmlUi's geometry has no anti-aliasing, so the UI is supersampled (`RenderSettings::uiSupersampling`, 2 on Windows, 1 on Android): drawn into a target twice the view's size (the view copied in nearest, so backdrop filters see the game) and averaged back down; its effect targets are that size too (about 50 MB each at 2400x1300).
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
