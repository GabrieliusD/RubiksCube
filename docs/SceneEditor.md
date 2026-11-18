# Scene editor plan

Goal: turn the CherryEngine editor into an Unreal-style editor. You open a game
**project**, build **scenes** by placing and editing entities in the viewport, and
press **Play** to run the current scene as the game. **Stop** returns to the scene
exactly as you left it.

Status: M1 to M6 are implemented on `features/editor-projects`. Some decisions changed during implementation:

| Planned | Built instead | Why |
|---|---|---|
| Asset GUIDs in `.meta` sidecar files | Assets referenced by their path under `Content/` (`Meshes/rock.glb`, `Materials/rock.mat`; built-ins `engine/Cube`) | Simpler, readable scene files, no sidecars; `*.meta` is in `.gitignore` anyway. Renaming a file breaks references to it. |
| ImGuizmo | A small gizmo of our own (`Editor/Gizmo.cpp`) | No third-party code fetched outside vcpkg, and no dependency on an ImGui fork. |
| Undo commands per edit | Whole-scene JSON snapshots, committed when a drag ends | Covers every kind of edit with no per-command code; scenes are small. |
| Separate `WorldTransform` component | `Scene::WorldMatrix(entity)`, computed from the `Parent` chain when needed | No per-frame transform system to keep in sync. |
| Unknown components stored per entity | Same (`UnknownComponents`), written back on save | |

The sections below are the original design; see the table above for where the code differs.

## Decisions taken

| Topic | Decision |
|---|---|
| Projects | One game = one project, stored in this repo (`Rubik/`, `Undertime/`), described by a `.cherryproject` file. The editor has one project open at a time. |
| Building | The editor's **Compile** button runs `cmake --build … --target <module>`. It also reloads when a DLL built from Visual Studio changes. |
| File formats | JSON for projects, scenes, materials and `.meta` files (nlohmann-json through vcpkg). |
| Play in editor | Play runs a **copy** of the edited scene, made by saving it to memory and loading that. Stop throws the copy away. |
| Game code | Games keep `IGameModule` (game-wide logic, like Unreal's GameMode). They also register **components and systems** that act on entities placed in scenes. |

## What exists today, and what is missing

- **ECS (`Scene/ECS.h`):**
  - Components are keyed by type-name string, so the keys work across DLLs.
  - Limits: 5000 entities and 32 component types (a `bitset<32>` signature). Both are too low for an editor; raise them to 64k entities and 64 types.
  - Entity ids are reused and change between runs, so scene files can't store them. Scenes need their own stable id (GUID) per entity.
  - Missing: listing an entity's components, and copying or saving components generically.
- **Components hold runtime data:** `MeshRenderer` (mesh and material handles), `VoxelVolumeRenderer` (volume handle) and `RigidBody` (`BodyId`). These handles exist only while the game runs. Saved data has to refer to assets and settings instead, and the runtime handles get created from them.
- **One scene and one physics world, owned by `Engine`.** Games reach them through `engine.GetScene()` and `GetPhysics()`.
- **Transforms are world-space only.** There's no parent/child hierarchy. The renderer reads `Transform::Matrix()` directly (`Renderer.cpp:850`, `:889`).
- **No asset system.** Meshes, materials and volumes are created from code.
- **Editor (`EditorLayer`):** a dockspace with Viewport, Stats and Game panels. There's no hierarchy, inspector, gizmo, selection or editor camera.
- **Module loading lives in `CherryLaunch/main.cpp`.** `Engine::Run(IGameModule&)` binds a single game for the whole run.
- **Content folders collide.** Every game's `Content/` is copied into one shared `bin/<Config>/Content`, so files with the same name overwrite each other.

## Architecture

### Project

`Undertime/Undertime.cherryproject`:

```json
{
  "name": "UNDERTIME",
  "module": "UndertimeGame",
  "content": "Content",
  "startupScene": "Content/Scenes/Mine.scene"
}
```

- **Opening a project:** `CherryLaunch.exe <path>.cherryproject` opens the editor on that project.
- **With no argument:** the editor shows a project picker with recent projects, stored in `%LOCALAPPDATA%/CherryEngine`.
- **Legacy argument:** `CherryLaunch.exe UndertimeGame.dll` still works, as a project with no scenes.
- **Engine path API (new):**
  - `Engine::Project()`: name, root and module.
  - `ContentPath(rel)`
  - `SavedDir()`: holds the editor layout (`imgui.ini`), editor settings and the last-open scene.
- **Content:** read directly from the project folder in the editor. Packaged builds copy only their own project's Content.

### Module host

- **The editor owns the game DLL.** It loads a copy of it (`UndertimeGame.reload-N.dll` plus a renamed PDB copy) from `Saved/Modules/`. Builds never hit LNK1104, and reloading is possible.
- **Engine and editor run with no game loaded.** This is the "project has no module yet" state.
- **Reload sequence:**
  1. Stop Play if it's running.
  2. Save the edit scene to memory.
  3. Unregister the module's types.
  4. Wait for the GPU to go idle.
  5. Unload the DLL.
  6. Copy and load the new DLL.
  7. Register its types again.
  8. Load the scene back from memory.

  Going through the saved form means a component whose layout changed still loads: fields are matched by name.
- **Unloaded game components are kept.** When a scene uses components whose module isn't loaded, or failed to compile, their data is kept as raw JSON. Nothing is lost.

### Worlds

`World` = one `Scene` + one `PhysicsWorld` + the main camera.

- **Edit world:** the scene being edited. No game logic or physics runs in it, except systems that opt in to run in edit mode (for previews).
- **Play world:** made on Play by saving the edit world and loading the result.
  1. Runtime handles are created: assets resolve to renderer handles, and rigid-body descriptions become Jolt bodies.
  2. `IGameModule::OnStartup` runs.
  3. Then each system's `OnPlayBegin` runs.
- **Stop:** the module's `OnShutdown` runs, then the play world is destroyed and the viewport shows the edit world again.
- **Pause and Step:** skip, or run once, `OnUpdate` plus physics.
- **Existing game code keeps working:** `engine.GetScene()` and `GetPhysics()` return the **active** world.

### Reflection and serialization

- **Type registry:** one global `TypeRegistry` holds a `ComponentInfo` for each component type.
- **What `ComponentInfo` contains:**
  - a name;
  - fields (name, type, offset, editor hints: range, color, read-only);
  - flags (`Saved`, `RuntimeOnly`, `HiddenInInspector`);
  - generic add, copy, save, load and draw-inspector functions.
- **Registration macro:**

  ```cpp
  CHERRY_REFLECT(Transform,
      CHERRY_FIELD(position)
      CHERRY_FIELD(rotation, EditAsEuler)
      CHERRY_FIELD(scale))
  ```

- **Field types:**
  - `bool`, `int`, `uint`, `float`;
  - `float2`/`float3`/`float4`, quaternion, color;
  - string, enum;
  - `EntityRef` (saved as a GUID);
  - `AssetRef<T>` (saved as an asset id).
- **New built-in components:**
  - `EntityInfo`: GUID, name, enabled.
  - `Parent`: entity ref, so `Transform` becomes local.
  - `WorldTransform`: runtime only, filled by a transform system before rendering. The renderer switches to reading it.
  - `PointLight`: the `PointLight` type already exists in `RenderTypes.h`.
- **Saved descriptions instead of handles:**
  - `MeshRenderer` keeps runtime `mesh`/`material` handles and gains `AssetRef` fields that fill them in. Code-made meshes (UNDERTIME chunks) set the handle directly and are marked not saved.
  - `RigidBody` keeps the runtime `BodyId`. A new saved `RigidBodyDesc` (shape, half extents, mass, static or dynamic) creates the body at Play.
- **Scene file:** a list of entities, each with its GUID and a component map, plus scene settings (sun, ambient, sky, exposure, which today are global `RenderSettings`).

### Assets

- **Asset ids:** an asset is any file under `Content/` with a `.meta` sidecar. The sidecar holds a 64-bit `AssetId` and import settings.
- **Asset manager:** scans `Content/`, maps ids to paths, loads assets with reference counting into renderer handles, and reloads them when the file changes.
- **First asset types:**
  - mesh: glTF through cgltf, plus built-in cube, sphere and plane;
  - material: `.mat` JSON;
  - texture: the existing DDS loader, plus PNG through stb_image;
  - scene.
- **Later:** voxel models.

### Editor tools

- **Toolbar:** Play, Pause, Step, Stop, Compile, and the module status (loaded, compiling, error).
- **Hierarchy:** a tree with create, delete, duplicate, rename and drag-to-parent.
- **Inspector:** built from the type registry, with Add Component.
- **Viewport:**
  - a separate editor fly camera (right mouse button plus WASD);
  - click to select, starting with `Engine::Raycast` against mesh bounds and later an id buffer;
  - move, rotate and scale gizmos with snapping, using ImGuizmo vendored into `thirdparty/` next to imgui;
  - selection outlines using the existing debug lines.
- **While playing:** the viewport shows the game camera. Shift+F1 ejects to the editor camera.
- **Content browser:** folders and assets. Drag a mesh into the viewport to place it.
- **Log panel:** captures `CHERRY_LOG`/`CHERRY_ERROR` and build output.
- **Undo/redo:** each edit is a command that stores a component's or entity's saved form before and after the change.

### Game API additions (all new)

```cpp
class IGameModule {
    // existing: GetConfig, OnStartup, OnUpdate, OnImGui, OnShutdown (Play only)
    virtual void RegisterTypes(TypeRegistry&) {}   // components + systems, on every load
};

class ISystem {
    virtual void OnPlayBegin(World&) {}
    virtual void OnUpdate(World&, float dt) {}
    virtual void OnPlayEnd(World&) {}
    virtual bool RunsInEditMode() const { return false; }
    virtual void OnEditUpdate(World&, float dt) {}  // previews, gizmos
};
```

## How the games move over

- **Day one, no game changes:** Play starts from an empty or saved scene and the game's `OnStartup` builds its world as it does now. Both games keep working in every milestone.
- **Rubik:** cube, camera and lights become placed entities. A `RubikCube` component holds the puzzle settings.
- **UNDERTIME:** the rock stays procedural.
  - **Scene contents:** `CaveWorld` (seed, zones, render mode), `PlayerStart`, `Lantern`, lights and handmade props.
  - **Generation:** happens at Play.
  - **Edit-mode preview:** a "Generate preview" button in the inspector builds the volume in the edit world, so you can place things against the real caves.
  - **Fragments and chunk meshes:** created by code at runtime and never saved.

## Milestones

Each milestone builds, runs, keeps both games playable, and ends with a report on what to test. Every engine change waits for your OK.

**M1: Projects and module host** (~1–1.5 weeks)
- **Project:** `.cherryproject` files for Rubik and UNDERTIME, an Open Project picker with recent projects, and the engine path API.
- **Per-project folders:** Content is read from the project folder, and each project has its own `Saved/` (with its own `imgui.ini`). This also fixes the shared Content folder.
- **Module host in the engine:** loads a copy of the game DLL, and engine and editor run with no game.
- **Play, Pause and Stop:** Play runs `OnStartup` on a fresh play world, and Stop runs `OnShutdown` and clears it.
- **Compile and reload:** a Compile button that runs cmake, and automatic reload when the DLL changes.
- **Checks:** repeated play/stop must not leak. Check entity count, physics bodies and GPU descriptors in Stats.

**M2: Scene core** (~1.5–2 weeks)
- **ECS:** raise the limits, and add a way to list an entity's components.
- **Reflection:** `TypeRegistry` and reflection for the built-in components.
- **New components:** `EntityInfo`, `Parent`, `WorldTransform` and the transform system. The renderer switches to world transforms.
- **Scenes on disk:** save and load scenes, and play from a copy of the edit scene. The startup scene opens with the project.
- **Editor:** hierarchy, inspector and the editor fly camera.
- **Checks:** a save → load → save round trip produces the same file (unit test, pure C++).

**M3: Editing tools** (~2 weeks)
- **Selection:** click to select, with outlines.
- **Gizmos and snapping:** ImGuizmo.
- **Entity editing:** create, delete, duplicate and parent.
- **Undo/redo**, the log panel, and scene settings moved out of global `RenderSettings`.

**M4: Assets** (~1.5–2 weeks)
- **Asset system:** `.meta`, asset ids, the asset manager, and reload on file change.
- **Asset types:** glTF meshes, built-in primitives, materials and textures.
- **Editor:** a content browser with drag-to-place, and `AssetRef` fields in the inspector.

**M5: Game API and hot reload** (~1–1.5 weeks)
- **Game types:** `RegisterTypes` and `ISystem`, with game components showing in the inspector.
- **Hot reload:** keeps the scene through save and reload, and keeps components it doesn't know.
- **Edit mode:** systems that opt in run in edit mode.

**M6: Port the games and package** (~1–2 weeks)
- **Rubik:** built as a scene.
- **UNDERTIME:** `CaveWorld`, `PlayerStart` and `Lantern`, plus the edit-mode cave preview.
- **Packaged run:** the launcher runs the startup scene in game mode with no editor. The monolithic build still links.

Total: roughly 8–11 weeks.

## New dependencies

- nlohmann-json: vcpkg.
- cgltf and stb (stb_image): vcpkg or vendored.
- ImGuizmo: vendored into `CherryEngine/thirdparty/`, because it has to build against the engine's own imgui copy.

## Risks and open points

- **Hot reload and engine headers:** a game DLL compiled against changed engine headers needs the engine rebuilt too. Changing engine code still means restarting the editor.
- **Code that assumes one scene:** game code that keeps `Entity` ids or handles across Play sessions will break. Everything has to be re-created in `OnStartup` (UNDERTIME already does this).
- **Physics during edit mode:** none. Rigid bodies exist only in the play world.
- **Gizmos on rigid bodies:** editing transforms of entities with rigid bodies works only in the edit world, where physics doesn't overwrite `Transform`.
- **Click selection:** until an id buffer exists, voxel volumes and code-made meshes may be hard to pick by clicking. Select them in the hierarchy instead.
