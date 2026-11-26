# Android support for CherryEngine, and the Dice game as an APK

Status (2026-11-25): **A0–A5 done** on branch `features/android`, checked on an Android 12 emulator, and the user tested it on a phone on 2026-11-26: it works. A2 and A3 were re-scoped (see "Change of approach" below).

| # | Milestone | State |
|---|---|---|
| A0 | Engine compiles for Android | Done. `Tools/Android/Build.ps1 [-Config Release] [-Configure]` builds `out/build/android-arm64/CherryLaunch/<Config>/libgame.so` (engine + Dice, arm64). Windows Debug, Release and monolithic build without new warnings; all four test suites pass; Dice still runs (screenshot with the pretend pointer). Not run on a device: nothing is drawn yet and there is no APK. |
| A1 | Platform layer, APK | Done, on an x86_64 emulator (Android 12, `Cherry_API_32_x64`); no phone yet. `Tools/Android/Package.ps1 -Project Dice/Dice.cherryproject -Abi arm64-v8a,x86_64 [-Install] [-Run]` makes `out/android/DiceGame-Debug.apk` (35.6 MB). On the emulator: the game files unpack on first start (not again after), the project and scene load, Dice deals its board and plays at a paced 60 fps (frames logged every 5 s); Home and back keeps the process; density and font-scale changes don't restart it; when the activity is destroyed the play session stops and the engine exits with 0. The screen is black: nothing is drawn until A3. |
| A2 | Vulkan renderer: device, swapchain, ImGui | Done, on the emulator (Vulkan 1.3 through the host's RTX 4070). `Renderer/Vulkan/VulkanRenderer.cpp` draws the Dice HUD and debug panel over the sky colour at 60 fps; Home and back releases and recreates the surface in the same process. No 3D yet. |
| A3 | Dice's 3D on Vulkan | Done, on the emulator. The board, dice (atlas texture, tint), felt grid, wooden base and shadows render; a swipe launches dice that tumble, glow when ready and throw sparks; 60 fps. Background colour identical to the Windows build (#14161a); the rest compared by eye only (tops, sides and felt read the same). arm64 Release builds; not yet seen on a phone. |
| A4 | Dice playable by touch | Done on the emulator: Start by tap, swipes launch dice (13,045 points in one session), Home and back mid-session keeps it, the "Time's up" card with its stats, 60 fps; the UI is scaled to 440 dpi (x2.75) in Roboto; the debug panel starts folded; the AAudio stream opens ("Dice: audio started") but the sound hasn't been listened to. Windows: Segoe and XAudio2 as before, `DiceTests` pass. |
| A5 | Project to APK in one command | Done. `Tools/Android/Package.ps1 -Project Dice/Dice.cherryproject [-Config Release] [-Abi arm64-v8a,x86_64] [-Install] [-Run]`, or the editor's **File > Export > Android APK (debug / release)** (the same script in the background, output in the Output panel). Release, both ABIs: `out/android/DiceGame-Release.apk`, 12.1 MB, label "Dice" with its icon, installs and runs on the emulator. Projects whose game doesn't build for Android are refused with a message. Not checked: signing with a real keystore (none exists yet), the menu clicked by hand (its command line was run as is). |

Goal: `Tools/ExportAndroid.ps1 -Project Dice/Dice.cherryproject` produces an installable,
signed `Dice.apk` that plays the dice game on an arm64 phone. Windows (D3D12, editor, hot
reload) keeps working exactly as today after every milestone.

## Where the engine stands (survey)

| Area | Today | Android problem |
|---|---|---|
| Graphics | D3D12 only. `RHI/` is a thin wrapper that hands out `ID3D12*` objects; `Renderer.cpp` (2.5k lines) records D3D12 commands directly (~256 lines touch `ID3D12`/`D3D12_`/`cmd->`). `Renderer.h` is pimpl: only `Initialize(HWND, ...)` leaks. | No D3D12. Needs a Vulkan backend. |
| Shaders | HLSL SM 6.6, compiled at runtime by DXC, bindless through `ResourceDescriptorHeap[]` in 17 shaders. | No DXC on device. `ResourceDescriptorHeap` maps to Vulkan only through `VK_EXT_mutable_descriptor_type`, which few phones have. |
| Window / input | `Core/Window` and `Core/Input` are Win32 (`HWND`, messages). ImGui uses `imgui_impl_win32` + `imgui_impl_dx12`. | Need `ANativeWindow`, touch, `imgui_impl_android` + `imgui_impl_vulkan` (both upstream). |
| Files | `std::filesystem` everywhere; Content read from the source tree; Shaders copied next to the exe. | Content lives inside the APK (`AAssetManager`); writable data goes to the app's internal storage. |
| Modules | `ModuleHost` (`LoadLibrary`), `BuildProcess`, editor, shader graph editor, DXC. | Not wanted on device. The monolithic build already links one game statically. |
| Libraries | vcpkg: directx-dxc, Jolt, nlohmann-json, cgltf, stb. DirectXMath comes from the Windows SDK. | Jolt, json, cgltf, stb build for `arm64-android`. DirectXMath is header-only with NEON support; take vcpkg's `directxmath` port on Android. DXC is host-only. |
| Dice | Pure C++ sim (`DiceSim`, `DiceControls`, `DiceVisuals`), ImGui HUD, `MeshInstances`, particles. Audio is XAudio2 (`DiceAudio.cpp`). Fonts load from `C:\Windows\Fonts`. Mouse hover drives everything. | Audio, fonts and input need replacing; the sim ports as is. |

What Dice actually uses from the renderer is small: opaque mesh pass (instanced
`MeshInstances`, built-in surface shader), two shadow cascades, particles (additive), FXAA,
tonemap with `toneCurve = false`, ImGui. Sky, SSAO, bloom, clouds, light shafts, edges, eye
adaptation and voxels are all switched off (`DiceModule.cpp:217-227`). **The Android renderer
only has to cover that subset first.**

Toolchain already on this PC: Android SDK (platform 34, build-tools 34), NDK 28.1, JDK 17,
Android Studio. The SDK's CMake (3.10) is too old; use Visual Studio's CMake + Ninja.

## Change of approach (2026-11-25, during A2)

A2 was "put a backend-neutral RHI under the D3D12 renderer, then add Vulkan under it". Instead
the mobile renderer is **a second implementation of the same `Renderer` API**
(`Renderer/Vulkan/VulkanRenderer.cpp`, as `NullRenderer.cpp` is), with its own small HLSL shaders
in `Shaders/Mobile` (classic descriptor sets, no bindless), covering what the Android games use.
Why: the D3D12 renderer, and with it Windows, UNDERTIME and Rubik, isn't touched at all (no risk
to the desktop look), and the phone gets pictures much sooner. The cost: features added to the
D3D12 renderer don't appear on mobile by themselves. A mobile forward renderer beside a desktop one
is a common split.

- **A2 (new):** Vulkan device, swapchain (surface loss and recreation), ImGui.
- **A3 (new):** Dice's 3D: meshes, materials and textures, the instanced mesh pass (`MeshInstances`
  with tint), sun + ambient lighting, shadows fitted to the board, particles, tonemap, FXAA.
- Bindless (decision 3 below) is dropped for now; the Windows-side Vulkan step (decision 2) too:
  the emulator runs Vulkan through the host GPU.

**What A2 built:** `VulkanRenderer.cpp` (Vulkan 1.1 instance, first graphics queue, FIFO
swapchain in UNORM, two frames in flight, deferred deletion, one-shot uploads); an ImGui renderer
written for ImGui 1.92's texture protocol (no backend was vendored for Vulkan);
`Renderer::SetNativeWindow` (the surface replaced or gone; Window's surface handler calls it, the
D3D12 renderer ignores it); `android_main` waits for the first surface before the engine starts.
Shaders: `Shaders/Mobile/*.hlsl` compiled to SPIR-V headers at build time by the host DXC
(`directx-dxc` is now a vcpkg *host* dependency; on Windows host and target are the same triplet).
Pre-rotation is left to the compositor (Android reports `SUBOPTIMAL`; ignored).

**What A3 built:** Mobile shaders `Mesh.hlsl` (the built-in surface with the D3D12 renderer's GGX
sun, 3x3 PCF shadow and sky/ground ambient), `Shadow.hlsl`, `Particles.hlsl` (camera-facing
quads, square or round, covering sorted back to front, then additive), `Post.hlsl` (tonemap:
exposure, ACES or clip, sRGB encode; FXAA; copy), sharing `Common.hlsli`. Renderer: GPU meshes
(always the full 48-byte vertex), textures with CPU-built mips (averaged in linear light for sRGB)
and anisotropic filtering, one descriptor set per texture; per frame a constants buffer, instance
and particle storage buffers; mesh entities and `MeshInstances` grouped into runs of one mesh and
material (one instanced draw each); passes: shadow (2048, fitted to `ShadowSettings::bounds*` like
D3D12, else to the view out to `farDistance`), scene into RGBA16F with a transient depth, tonemap
into RGBA8, then FXAA (or a copy) into the swapchain under ImGui. Not drawn yet on Vulkan: voxel
volumes, surface shaders (materials draw with the built-in surface), point and fill lights, fog,
sky, SSAO, bloom, light shafts, grading, eye adaptation, debug lines, view models, glass.

**What A4 built (and differences from the plan):** no new libraries were downloaded.
- **Audio:** AAudio (part of Android) instead of miniaudio: `DiceAudio` keeps the synthesis and
  Play's throttling shared, with the device part per platform (XAudio2; AAudio with a 32-voice
  mixer in the stream's callback, 48 kHz float, low latency).
- **Fonts:** the system's Roboto instead of a bundled font. Android 12's Roboto is a variable font,
  which ImGui reads at its regular weight, so the HUD's semibold/bold/black are regular on Android.
- **Touch:** a finger is the pointer only while it touches; lifted, the pointer moves off the view
  (in `Input` and ImGui), so nothing stays hovered. ImGui knows it's a touch screen.
- **Scale:** `android_main` makes the ImGui context with the sizes and fonts scaled by the screen
  density / 160 (as the Windows renderer scales by monitor DPI); the renderer uses it.
- **Panel:** Dice's debug panel starts folded on Android (tap its title to open it).

Left for later: Vulkan pre-rotation (the compositor rotates now), immersive mode (the navigation
handle shows), drawing under the display cutout (a black band on the cutout side), the audio
stream's disconnect callback (headphones), a mobile shadow-map size, and checks on a real phone.

**What A5 built (and differences from the plan):**
- `Package.ps1` grew the project check (`$AndroidGames`: DiceGame only), the launcher icon
  (`android.icon` in the project file; Dice's is `Dice/Icon.png`, a drawn die) and release signing
  from `CHERRY_ANDROID_KEYSTORE`, `CHERRY_ANDROID_KEYSTORE_PASSWORD`, `CHERRY_ANDROID_KEY_ALIAS`,
  `CHERRY_ANDROID_KEY_PASSWORD` (environment only; without them a release APK is signed with the
  debug key and the script says so).
- `Engine::ExportAndroid(release)` / `IsExportingAndroid()` run the script through a second
  `BuildProcess` (arm64), and `EditorLayer` has the File > Export menu.
- **No cook step:** the mobile shaders are compiled into the library, and the Vulkan renderer
  draws every material with its built-in surface, so there is nothing per game to cook yet. The
  cook comes back when the mobile renderer supports surface shaders (`.mat` / `.shadergraph`).

## What's next (not started)

- Vulkan pre-rotation, immersive mode, drawing under the cutout, the audio disconnect callback.
- A release keystore for store builds, and an `.aab` for the Play Store.
- More of the renderer on Vulkan (surface shaders, point lights, fog, sky, voxels) before Rubik or
  UNDERTIME can go to Android.

## Decisions to make (recommendations first)

1. **Graphics API: Vulkan** (1.1 + `descriptorIndexing`, standard on Android 10+ GPUs:
   Adreno 6xx+, Mali-G7x+, Xclipse). GLES 3 has no bindless and would mean a second renderer
   design; a third-party abstraction (bgfx, Diligent, SDL_GPU) means rewriting all of
   `Renderer.cpp` and SDL_GPU has no bindless anyway.
2. **The Vulkan backend also runs on Windows.** Debug it on the desktop with validation layers
   and RenderDoc, compare screenshots against D3D12 with `Dice/Tools/Screenshot.ps1`, and only
   then go to a phone. `--rhi vulkan` on the launcher picks it.
3. **Bindless through typed arrays, not `ResourceDescriptorHeap`.** A new
   `Shaders/Shared/Bindless.hlsli` defines `CHERRY_TEX2D(i)`, `CHERRY_BUFFER(i)`, ... On D3D12 they
   stay `ResourceDescriptorHeap[i]`; on SPIR-V they index unbounded arrays
   (`Texture2D gTex2D[]`, `ByteAddressBuffer gBuffers[]`, ...) in one descriptor set. The C++
   side keeps one index space per resource kind.
4. **Shaders are compiled offline for Android.** DXC on the Windows host (`-spirv
   -fspv-target-env=vulkan1.1`) during the build; the APK carries `.spv` files and a manifest
   keyed by shader + variant. The runtime `ShaderCompiler` is replaced by a loader on Android.
   Surface shaders a game uses are listed by a **cook** step (see A5) so nothing compiles at run
   time.
5. **Packaging: Gradle + GameActivity** (AndroidX games library), with
   `externalNativeBuild { cmake }` pointing at the repo's root `CMakeLists.txt` with
   `CHERRY_MONOLITHIC=ON`, `CHERRY_ACTIVE_GAME_TARGET=DiceGame`, `CHERRY_EDITOR=OFF`. One
   `libgame.so` (one name for every game, so one Gradle template fits all), arm64-v8a only, minSdk 29.
6. **Audio: miniaudio** (single header, AAudio/OpenSL on Android, WASAPI on Windows), so
   `DiceAudio` has one implementation instead of an XAudio2 one plus an Android one. It stays
   in the game, as today (the engine has no audio).

## Milestones

Each one builds and runs on Windows (modular and monolithic) and ends with a report.
Everything under `CherryEngine/` needs the user's OK; these changes are the plan for asking it.

### A0. The engine compiles for Android (no window yet)

- `CMakePresets.json`: `android-arm64` (Ninja, NDK toolchain, vcpkg `arm64-android` triplet
  chained with `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`).
- `vcpkg.json`: `directx-dxc` only on Windows (`"platform": "windows"`), `directxmath` on
  Android.
- `CherryEngine/CMakeLists.txt`: split sources into common / `Platform/Windows` /
  `Platform/Android`; `CHERRY_EDITOR` option (default ON on Windows, OFF on Android) dropping
  `Editor/`, `ModuleHost`, `BuildProcess`, `ShaderGraph` editor, imgui-node-editor, DXC;
  MSVC-only flags behind `if(MSVC)`; `IMGUI_API` dllexport only for shared builds.
- `Core/Engine.cpp`: editor paths behind `CHERRY_EDITOR` (`MessageBoxA`, editor state,
  hot reload, Play/Stop).
- Code fixes found by clang: `fopen_s`, `wchar_t` paths (`Widen`), Win32 includes in headers
  (`Input.h`, `Window.h`, `Renderer.h`, `ShaderCompiler.h`).
- **Done when:** `cmake --preset android-arm64 && cmake --build` links `libgame.so` with stub
  window/renderer; all Windows tests still pass.

**What A0 built:**
- `CMakePresets.json` `android-arm64` (Ninja Multi-Config; Debug and Release); `Tools/Android/Build.ps1` finds Visual Studio's CMake, Ninja and vcpkg and the newest NDK.
- `vcpkg.json`: `directx-dxc` on Windows only, `directxmath` on Android.
- `CherryEngine/CMakeLists.txt`: `CHERRY_EDITOR` (ON on Windows, OFF elsewhere, a public define). Off Windows the editor, module host, build process, D3D12 RHI and renderer, DXC, DDS loader, node editor and ImGui's Win32/DX12 backends are left out, and `Renderer/NullRenderer.cpp` stands in for the renderer (handles, bounds and materials are kept; ImGui runs its frames; nothing is drawn). Tests build on Windows only.
- `Core/NoEditor.h`: what `Engine.cpp` uses from the editor, module host and build process, reduced to a packaged game's behaviour.
- Platform guards: `Log` (logcat; `cherry::ShowError` replaces `MessageBoxA`), `Project` (`SetExecutableDir`, the app's data folder), `Input` (Win32 parts behind `_WIN32`; new `OnPointerMove/OnPointerButton/OnWheel/OnKey` feeders), `Window` (an Android class around the `ANativeWindow`, driven by a pump the platform layer installs), `Renderer::Initialize(void* nativeWindow, ...)`, `MathHelper.h`, `Engine.cpp` (`_stricmp`, `swprintf_s`, `CreateProcessW`).
- `CherryLaunch/AndroidMain.cpp`: NativeActivity glue (`android_main`), lifecycle commands into `Window`, touch as the pointer and left button; links the game with `--whole-archive`.
- Dice: `GetEnv` instead of `_dupenv_s`, portable `sscanf`/font file check, `DiceAudio` silent off Windows (XAudio2 behind `_WIN32`).
- Two fixes clang needed: `ShaderGraph.cpp` (an `ordered_json` push ambiguity), `Assets.cpp` (`rfind` on a wide path).

**What A1 built (and where it differs from the plan):**
- **NativeActivity instead of GameActivity:** no Java code or AndroidX libraries at all (`android:hasCode="false"`), so the Gradle template needs only the Android Gradle plugin (8.6.1, Gradle 8.9 wrapper, compileSdk 34, minSdk 29). GameActivity can come later if text input or insets need it.
- **Unpacked files instead of reading through `AAssetManager`:** `Package.ps1` puts the project file and `Content/` in the APK with `files.txt` (a build stamp, then every path); `AndroidMain.cpp` copies them into the app's data folder when the stamp changes. The engine keeps using ordinary file I/O and `ExecutableDir()` is that folder. Shaders will come the same way (A3).
- `Android/`: the generic Gradle app (`app/build.gradle` takes `-PcherryAppId/Label/VersionCode/VersionName/Orientation/JniLibs/Assets/NdkVersion`). `.cherryproject` files get an optional `"android"` section (Dice: `com.cherryengine.dice`, "Dice", landscape).
- `Tools/Android/Package.ps1` (native build per ABI, staging in `out/android/<Module>/`, Gradle, copy to `out/android/<Module>-<Config>.apk`, `-Install`, `-Run`); `Build.ps1 -Abi arm64-v8a,x86_64`; `Common.ps1` finds the tools. Preset `android-x64` for emulators; its triplet `Tools/Android/triplets/x64-android-sse4.cmake` builds Jolt without AVX/AVX2/FMA (the emulator CPU stops at SSE4.2 + F16C; with them the library died at load with SIGILL). Both presets link libc++ statically.
- Lifecycle: the manifest handles every configuration change itself (the engine can't restart inside one process), so the activity is only destroyed when it finishes; `android_main` then ends the process. Back on Android 12+ moves a launcher app's root activity to the background instead of finishing it (system behaviour); the engine sleeps while there is no surface.
- Touch feeds ImGui as well as `Input` (mouse source "touch screen"). `NullRenderer` paces frames to 60 Hz when vsync is on. On Android, `Window::SetTitle` logs the title (fps, frame time) every 5 s.

### A1. Platform layer (Windows first, then Android)

- `Platform/Platform.h`: `NativeWindow` (opaque handle + size + DPI), events (resize,
  focus, pause/resume, surface lost/created), `FileSystem` (read-only content root, writable
  saved root), `Log` sink, time.
- Windows implementation = today's `Window`/`Input`/`Log` moved behind it; `Renderer::Initialize`
  takes `NativeWindow` instead of `HWND`.
- Android implementation on GameActivity: `android_main` → engine loop; `AAssetManager` reads
  for `Content/` and `Shaders/`; `Saved/` → `internalDataPath`; `__android_log_print`;
  `AInputEvent`/GameActivity motion events into `Input`.
- **Input on touch:** one finger = the pointer (`MousePosition`, `HasMouse` true while touching,
  `WasMousePressed` on touch down); two-finger pinch = `WheelDelta`. ImGui gets touch through
  `imgui_impl_android`.
- **Done when:** Windows unchanged; an APK opens full screen, logs frames, survives
  rotate / home / back, and quits cleanly.

### A2. A backend-neutral RHI (D3D12 behind it, no visual change)

The biggest and riskiest step.

- `RHI/` becomes an interface: `Device`, `CommandList` (render passes, pipelines, root/push
  constants, draw/dispatch, barriers, copies), `Buffer`, `Texture`, `Pipeline`, `SwapChain`,
  bindless index allocation, deferred release, `ExecuteAsync` uploads. Two frames in flight as now.
- Move today's code to `RHI/D3D12/`. Port `Renderer.cpp` off `ID3D12*` onto the interface
  (pass by pass; ImGui backend chosen by the RHI).
- Shaders switch to the `Bindless.hlsli` macros (D3D12 output identical).
- **Done when:** Dice, Rubik and UNDERTIME screenshots match the pre-change build pixel for
  pixel (or within FXAA noise); `CherryEngineTests` pass; frame times unchanged.

### A3. Vulkan backend on Windows, Dice's subset

- `RHI/Vulkan/`: volk + VMA (vcpkg: `volk`, `vulkan-memory-allocator`, `vulkan-headers`),
  one bindless descriptor set per kind, push constants for the root constants, dynamic rendering
  (or render passes where the device lacks it), timeline-free fences for broad device support.
- Offline SPIR-V: a CMake step runs DXC `-spirv` for the shaders Dice needs (mesh pass +
  compact variant, shadow + compact, particles, FXAA, tonemap, blit, debug lines) and writes
  `Shaders/spirv/*.spv` + manifest. Passes not yet ported are skipped when the backend lacks
  them (logged once).
- `CherryLaunch.exe Dice/Dice.cherryproject --play --rhi vulkan`.
- **Done when:** Dice under Vulkan matches D3D12 screenshots (`Screenshot.ps1 -Rhi vulkan`):
  board, shadows, mid-tumble dice, sparks, HUD; no validation errors; 1,600 dice at well over
  60 fps.

### A4. Dice runs on a phone

- Vulkan surface from `ANativeWindow`; swapchain recreated on surface loss and rotation;
  pre-rotation handled (`VK_SURFACE_TRANSFORM`); FIFO present (vsync on, Dice's `vsync = false`
  ignored on Android).
- Dice changes: `DiceAudio` on miniaudio; a bundled open-licensed UI font in
  `Content/Fonts/` replacing Segoe UI (Inter or Noto Sans, OFL) on both platforms or Android
  only (**your call**: same look everywhere vs. keeping Segoe on Windows); ImGui scaled by
  the display density; debug panel behind a corner button; landscape lock.
- Mobile defaults in a preset (`Content/Presets/mobile.json`): shadow map 1024, FXAA on.
- **Done when:** installed with `adb install`, it plays a full session by touch: dragging the
  finger launches dice, Start/Restart work, sound plays, score card shows; pause/resume keeps
  the session; measured fps and frame times on the test phone go into this doc.

### A5. Export: project → APK in one command

- `.cherryproject` gets an `"android"` section: `packageId` (e.g. `com.cherry.dice`), `label`,
  `version`/`versionCode`, `orientation`, `icon` (PNG in Content).
- `Android/` folder at the repo root: one generic Gradle app template (GameActivity, manifest
  filled from the project file, `assets/` filled by the export).
- **Cook:** the export runs the game on Windows headless for one frame
  (`--cook`), records every surface shader, material and variant it created, compiles them to
  SPIR-V and copies Content + `.spv` + manifest into the APK's assets. That keeps working when a
  game uses `.mat`/`.shadergraph` shaders later.
- `Tools/ExportAndroid.ps1 -Project <file> [-Release] [-Install] [-Run]`: configure + build
  native, cook, Gradle `assembleDebug`/`assembleRelease`, sign (debug keystore; release keystore
  path and passwords from environment variables, never the repo), optional `adb install` and
  launch, output in `out/android/<Name>-<version>.apk`.
- Editor: **File → Export → Android APK...** runs the script through `BuildProcess` and shows
  its log.
- **Done when:** from a clean checkout, the one command makes a signed APK that installs and runs;
  the editor menu does the same.

### Later (not needed for Dice)

- The remaining passes on Vulkan (voxels, sky, SSAO, bloom, clouds, light shafts, eye
  adaptation), then Rubik and UNDERTIME on Android.
- Android App Bundle (`.aab`) for the Play Store, multiple ABIs, `ASTC` texture compression.
- Physics: Jolt already builds for Android; nothing to do until a game on Android needs it.

## Risks

- **A2 size.** Every pass in `Renderer.cpp` is touched. Mitigation: port pass by pass, keep
  screenshot comparisons for all three games after each.
- **Descriptor indexing on the test phone.** Check `vulkaninfo` / GPU Caps Viewer on the
  device before A3. Without `runtimeDescriptorArray` + partially bound, fall back to fixed-size
  arrays (Dice needs only a handful of textures and buffers).
- **SPIR-V differences from DXIL:** half precision, `SV_InstanceID` base (Vulkan includes the
  base instance; DXIL doesn't), Y flip of clip space (flip in the viewport), depth range.
  The `instances[gDraw.instanceIndex + SV_InstanceID]` read in the mesh pass needs care.
- **Tile-based GPUs:** load/store ops must be right (clear, don't load) or the frame pays
  bandwidth; FXAA + tonemap as separate full-screen passes cost more than on desktop.
- **Per-instance CPU cost** (`Port.md` M10: ~0.26 µs per instance on a 5900X) will be several
  times higher on a phone; fine at 1,600 dice, an issue past ~10k.

## What you'd need to provide

- A test phone (model / GPU), USB debugging on.
- The app id and name, and later a release keystore.
- OK for the engine changes in A0–A3 and A5 (all of them are engine work).
