# UNDERTIME (CLAUDE.md for Undertime/)

Guidance for working on UNDERTIME. The engine, the build and the branch rules are in the root `CLAUDE.md`.

## Architecture

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
- **The working loop (Docs/Hauling.md):** UndertimeModule methods split into their own files.
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

## Status and design

- **Design documents** (in `Undertime/Docs/`):
  - `Design.md`: decisions and why: the time rule, rewind rules, the loop and progression, eras, story, the valley and town, the visual style, the plan, playtest questions. Items are marked Decided, Proposed or Open. Read it before UNDERTIME feature work, and update it when the user changes a decision.
  - `Status.md`: what's built, what the user has tested, branches and commits, what's next.
  - `Changelog.md`: how each milestone was built, measurements, fixes after the user's looks. Add to it when a milestone is done, instead of writing build history into Design.md.
  - `NPCs.md` and `Hauling.md`: those systems' plans and status.
- **Current work:** on `master` (milestones 1–15, NPC milestones 1–5 and hauling H1–H12 done; most untested by the user; see `Status.md`). The biggest gaps: the mined cave isn't saved, rewinds, the watches and the ending (Design.md §3, §5).
- **User testing:** milestones 2–7 were tested by the user (2026-11-21).
- **Commits:** the user commits when they ask. The list is in `Status.md`.
- **Style:** everything is 0.25 m voxels. The cave is real (diggable) voxels; everything outside is voxel models baked to meshes. Placeholder art is generated as `.vox` files; MagicaVoxel is the editor for now, and a built-in voxel designer may replace it later.

## Working rules (from the user)

- **Never invent engine APIs.** Only call functions that exist. If a feature is missing, say so and propose the smallest engine addition or a workaround in game code. Wait for the user's OK before changing anything under `CherryEngine/`.
  - **Except:** engine additions the agreed visual style needs (Design.md §8, "Engine additions": sky, global shader inputs, lights, instancing, shadows, bloom and similar) are pre-approved, and so is the `.vox` voxel model pipeline. Name each engine change in the plan and the report.
- **Work in milestones.** Each one must build and run. Then stop and report what to test and what couldn't be verified.
- **Tuning numbers** all live in one place: `Config.h`.
- **Audio** is out of scope for now.
- **Branches:** feature work happens on `undertime/<topic>` branches (see "Branches and commits" in the root `CLAUDE.md`). Commit only when asked.
