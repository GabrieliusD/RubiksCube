# UNDERTIME changelog

How each milestone was built, what was measured and what was fixed after the user looked. Decisions live in `Design.md`; what's built and tested is in `Status.md`. Newest last, per milestone.

---

## Milestones 1–7 (branch `features/voxel-town`, tested by the user 2026-11-21)

1. **Voxel model pipeline (engine):** `VoxelModel`, `.vox` load and save, the mesh and collision bakers, the asset type, tests. A sample is at `Content/Models/Samples/LanternCrate.vox`.
2. **Engine visuals I:** sky, global shader inputs, nearest-8 lights, instancing.
3. **The valley:** terrain, collision, water, forest; walkable from the cave mouth.
4. **Engine visuals II:** sun shadows (meshes and voxels), bloom. Voxel volumes received shadows but didn't cast them until milestone 7.
5. **Camp era:** the valley shown in the editor, the generated placeholder `.vox` set, the era component, the camp placed in the editor. The clockmaker's cottage moved to the river east of the square (with a water wheel), and trails lead from the mine to the camp and on to the clockmaker.
6. **Settlement era:** its content and the switch between eras (the N key until the clock existed). Buildings get flat ground pads. Left out: the forest isn't cut back per era (stumps are placed instead), lanes exist in every era.
7. **Fixes:** the loose-block slowdown, voxel volumes casting shadows, anti-aliasing. The slowdown was piles of blocks never falling asleep in the physics; settled blocks now sleep. The cave mouth was redone after it: a craggy rock face with grassy ledges that can't be mined from outside, a rough 3 m portal, and per-pixel cave lighting so the valley stays lit when you look out.

## Milestones 8–12 (commit `e8bf6d6`)

8. **The outside clock and day/night:** the clock and its rate by depth, the sun, moon, sky and ambient through the day, lights at night, the HUD watch, eras from the clock, the summary when you come up. Town windows light up at night through `Materials/Town.mat`; the campfire and forge always glow. Seasons are not in.
9. **Light and depth:** daylight spreading into the cave from the mouth (a light volume), natural shafts, darkness in place of the black fog, SSAO, cloud shadows. Three shafts (into zones 1–3) with log grates at the surface; clouds drift and shade the land. The sun was left as the clock makes it (low and warm mornings and evenings).
10. **The mountain matches its face:** steep ground climbs in 2.5 m layers (rock risers, grass ledges); tall rock walls show the layers as bands of three rock shades, and the cave's face uses the same layers and shades. Rock outcrops stand on steep ground. Heightfield cliffs stay flat walls (no overhangs).
11. **Houses:** every building has an open doorway and furniture for its purpose (counters, shelves and barrels in the store and trading post; desks and a safe in the offices; a bar in the saloon; a forge, anvil and tool rack in the smithy; beds, a table and a stove in homes; a workbench and wall clocks at the clockmaker). A lamp hangs in each and glows at night. Windows are real glass. Indoors the sky light drops to 30%; sunlight comes in through windows and doors.
12. **Particles (engine):** emitters on entities or set by the game, billboards lit like the scene. First uses: dust in the shaft beams (lit only where the sun patch is), chimney and campfire smoke, forge sparks, dust when rock breaks or a cave-in falls. Simulated on the CPU (a few thousand particles), not soft against geometry.

**Polish after milestone 12:**
- **Cloud seams:** the cloud noise hashed corner values with floating-point maths, which the shader compiler may reorder, so neighbouring cells disagreed and straight seams showed. It uses an integer hash now.
- **Clouds follow outside time:** wind × how much faster time runs, so they race past the mouth in the cave. They blur into a veil once a cloud would cross the sky in under 3 seconds (`kCloudBlurSlow`/`kCloudBlurFast`).
- **Readable cloud shadows:** smaller clouds (130 m), crisper edges, 80% of the sun blocked, and a stronger sun (1.3) against a weaker sky light (day ambient 0.2).

## Loose blocks: no block is ever lost (2026-11-22)

- **Settling:** a block that stays put for half a second (jitter in a heap counts as staying put) settles into the pile of its 1 m cell: one mesh and one static body per cell. A pile collects settling blocks for 0.3 s and is then rebuilt with all of them, within 2 ms a frame.
- **Waking:** mining under or beside a pile wakes those blocks, and whatever rests on them.
- **Limits:** only blocks in motion have their own body (at most 2,500; past that the stillest settle early). A block that falls out of the world is put back at the mine mouth.
- **Numbers:** in Release, 8,000 blocks thrown out with a random push and spin settled into 180 piles within about 4 s, with physics at 0.03 ms a frame afterwards. Rebuilding a pile of 64 blocks takes about 1.3 ms.
- **Fixed after the user's first test:** blocks only settled once the physics put them to sleep, and a heap being mined rarely sleeps (3,500 blocks stayed live, physics 20 ms). Each pile rebuild also cost about 7 ms, because the engine waited for the GPU on every upload. Now blocks settle by staying put, and engine uploads no longer wait.
- **Test flags** in the editor's Game panel ("Loose block tests"): the old cap (past 3,000 the oldest crumble away), piles on or off, solid falling blocks, solid piles; buttons that drop 2,000 or 10,000 blocks.

## Milestone 13: Visual polish II (2026-11-22)

Built in this order (foliage and the era grade before eye adaptation and water, at the user's request).

1. **Light shafts (engine):** one half-size compute pass (`Shaders/LightShafts.hlsl`) marches each view ray through the sun's shadow maps and the clouds, and in the cave only where the light volume lets the sun in. The air scatters mostly towards the sun (`kShaftDensity`, `kShaftAnisotropy`): a golden haze with beams towards a low sun, thinning with distance. The cave column is subtle (the user asked to focus on outside).
2. **Living foliage:**
   - **Sway:** `Materials/Foliage.mat` (`Content/Shaders/Foliage.hlsl`): a vertex offset along the wind, growing with height squared, gusts rolling downwind and a quicker flutter. Wind is `ShaderGlobal(2)`. Shadows don't sway.
   - **Ground cover:** two grass tufts, flowers (yellow, white, purple, in patches), bushes, pebbles, scattered on flat grass clear of pads, paths and water, baked into the terrain region meshes (about 0.9 M triangles). Densities in `Config.h` ("Ground cover").
   - **Forest floor:** a Moss ground type where `Valley::ForestPatch` is wooded.
   - Engine: `#define CHERRY_SURFACE_BOUNDS_PADDING <metres>` for culling bounds.
3. **Colour grade per era (engine):** `RenderSettings::colorGrade` in the tonemap: white balance and saturation before the tone curve, contrast, lift and gain after, then a vignette. Eases to `kCaveGrade` over `kGradeCaveDepth` metres in the cave, and over `kGradeSeconds` on every change. The first tuned values (version 1), kept so they can be put back after experiments (`cfg::kEraGrades`, `cfg::kCaveGrade`; tint, lift and gain are red, green, blue; image `Images/era_grades_v1.png`):

   | Era | Tint | Saturation | Contrast | Lift | Gain | Vignette |
   |---|---|---|---|---|---|---|
   | Camp (warm sepia) | 1.08, 1.00, 0.84 | 0.78 | 1.06 | 0.035, 0.022, 0.000 | 1.00, 0.97, 0.90 | 0.30 |
   | Settlement (warm) | 1.05, 1.00, 0.92 | 0.92 | 1.05 | 0.015, 0.008, 0.000 | 1.00, 0.99, 0.96 | 0.24 |
   | Boomtown (rich) | 1.02, 1.00, 0.97 | 1.15 | 1.10 | 0, 0, 0 | 1.00, 1.00, 0.98 | 0.20 |
   | Industry (cold) | 0.92, 0.98, 1.08 | 0.78 | 1.12 | 0.000, 0.006, 0.022 | 0.97, 0.99, 1.02 | 0.26 |
   | Abandoned (faded) | 1.00, 0.98, 0.93 | 0.55 | 0.88 | 0.060, 0.055, 0.050 | 0.95, 0.95, 0.93 | 0.32 |
   | Gone (green) | 0.98, 1.02, 0.97 | 1.10 | 1.02 | 0.000, 0.004, 0.000 | 1.00, 1.00, 1.00 | 0.16 |
   | Cave (any era) | 1, 1, 1 | 1.00 | 1.04 | 0, 0, 0 | 1, 1, 1 | 0.30 |

   Blending: `kGradeSeconds` 1.5, `kGradeCaveDepth` 8 m. Eye adaptation with it: `kEyeKey` 0.2, exposure 1 to `kEyeMaxExposure` 2.2 (1.5 outside at night), `kEyeSpeedBrighter` 1.6, `kEyeSpeedDarker` 0.35.
4. **Eye adaptation (engine):** `Shaders/EyeAdaptation.hlsl`: average brightness (centre-weighted, 16x16 blocks), an exposure stored on the GPU that eases quickly towards brightness and slowly towards darkness; nothing is read back. UNDERTIME keeps it between 1 and `kEyeMaxExposure`.
5. **Water that flows:** `Content/Shaders/Water.hlsl` with a flow map (`ValleyScenery::BuildFlowMap`, one texel per metre: flow, depth, foam below falls). Two-layer flow-mapped ripples, calmer on the lake; more sky reflection at grazing angles; sun glints; shallow edges; foam down the falls fading downstream; spray particles at the falls nearest town (`kFallSprayMax`). Not done: lit windows and moonlight reflected at night (needs screen-space reflections), the bank reflected.
6. **Voxel edge highlights:** in screen space (`Shaders/Edges.hlsl`: convex edges from the view depth, silhouettes skipped), because surface shaders can't tell a block corner from a seam in a merged face. Fade by `kEdgeDistance`.

**Fixes after the user's looks (2026-11-22):**
- First: the waterfall flickered where its see-through steps overlapped the riverbed (water writes depth now; step faces 2 cm out with a lip); every small river step showed a foam wall (foam only on real falls); fast water stretched ripples into streaks (ripples carried at most about 1 m/s, fine ripples fade with distance, flow map smoothed); shadows shimmered as time passed (shadows step in 0.25 degrees, `ShadowSettings::direction`, while the sun moves smoothly).
- Second: walls of water where the land beside the river or lake was lower than the water (banks get a lip at water level); falls recognised by their own drop (carried in the face colour); fall faces 6 cm out (depth precision far away).
- Third: the ledges between the waterfall's drops churn with foam (columns within 1.5 m of a drop); foam is soft streaks carried by the current; river chunks are always fine blocks (the waterfall's coarse upper part didn't meet its fine lower part); shadows can fade between steps (an extra set of shadow maps; `kShadowStepDegrees`, `kShadowFadeSeconds`, J toggles).
- New debug keys: G flies, Shift runs, ] and [ speed time up and down.

## Milestone 14: Visual polish III (2026-11-22)

1. **Valley haze:** `RenderSettings::heightFog`: thickest on the valley floor, bluer like the sky, warmer towards the sun; the sky's horizon fades into it. A dawn mist around sunrise and a little at dusk. `Config.h` ("Valley haze").
2. **Mining feel:** hot streaking sparks on ore or rock too hard to break; crack lines on damaged blocks; cave-ins shake the camera with a rumble that grows with the rock that fell and fades with distance.
3. **The cave comes alive:** `Content/Shaders/CaveRock.hlsl` (`Materials/CaveRock.mat`; the old `Rock.shadergraph` is untouched): ore glints, Time crystal glows cyan and lights its surroundings (a point light per cluster near you), the strange watch pink, damp rock deeper down, cracks. Lamps hang in zone 1's high caverns (`MineLamp.vox`). Dust motes drift in the lantern light. Not done: wet rock by water (no cave water yet), timber props.
4. **Weather:** `WeatherAt(days)` (DayNight.h, tested): overcast and rain from the outside clock, the same for the same day (so a rewind replays it), changing over a day or two; ground stays wet after. Rain is stretched particles around you outside; wet ground darkens with puddles (`Content/Shaders/Ground.hlsl`). Racing days blur it to its average. Not done: snow, rain on roofs and in the water.

**Showcase cave** (after milestone 14, at the user's request): a fixed test hall inside the entrance (`Scenes/TestCave.scene`, `UNDERTIME_DEV_SHOWCASE=1`). It found two bugs at once: cracks never showed on a freshly built world's blocks, and crystal light sat at floor height. After the user's look (2026-11-23):
- Cracks redrawn as a few long lines across blocks instead of a crazed mesh per block; then as 1-pixel lines on an 8 x 8 grid per face whose crossing points on each block edge are shared with the neighbour, so they run across blocks (a few when light, a web when bad). Cracked blocks darken less (`kCrackShade`).
- Ore went from sub-block flecks, to per-block nuggets with a pixel bevel (looked like ingots), to rock with metal in it: the surrounding stone with metal in pixel clusters from a world-space noise, running as seams across blocks; only metal is glossy; copper has verdigris, silver tarnish and bright pixels, gold veins in pale quartz (`kOreRichness`, `kGoldRichness`, `kGoldQuartz`, `kCopperVerdigris`, `kSilverTarnish`).

**Rock relief** (2026-11-23, a trial): shading-only relief on the same pixel grid: chips of about 3 x 3 pixels, each tilted one of 8 ways or flat (damp rock less); pixels beside a crack lean into it, so it looks cut in. Fades out by 14 m. No textures, no engine change. `kRockRelief`, `kCrackGroove`, `kReliefFadeDistance` (0 turns each off). Could later go on cliffs and on town wood and stone; not on foliage, far grass or loose blocks.

## Milestone 15: Valley LOD (2026-11-23)

**As built:** 64 m regions at 0.25 m blocks up to 110 m from the camera, 0.5 m to 260 m, 1 m beyond (`kLodDistance`, `kLodHysteresis`); river chunks always 0.25 m; ground cover only on 0.25 m regions; trees and crags swap to halved (0.5 m) models past 150 m; collision 0.25 m within 40 m of the player. Mountain outcrops are crags modelled at four times the size in 0.25 m blocks (`Crag*.vox`). The valley builds in about 1.2 s (was 2.1 s). Not done: following the editor camera in the preview.

**Options to compare** (the user found the pop-in noticeable; all in the Game panel's "Valley detail"): distance presets, a crossfade between levels (engine: `MeshRenderer::dither`, over `kLodFadeSeconds`), compact vertices (engine: `MeshFormat::Compact`, 28 bytes instead of 48). Measured from above the valley on the development PC:

| Distances | Triangles | Full vertices | Compact vertices |
|---|---|---|---|
| LOD (0.25 m to 110 m, 0.5 m to 260 m, 1 m) | 3.3 M | 2.5 ms, 394 MB | 2.2 ms, 248 MB |
| 0.25 m to 250 m, then 0.5 m | 9.2 M | 2.7 ms, 1003 MB | 2.5 ms, 631 MB |
| 0.25 m everywhere | 18.7 M | 3.8 ms, 1977 MB | 3.4 ms, 1245 MB |

Loading takes about 1.2 s with the LOD and 3.4 s with 0.25 m everywhere. Defaults: the LOD, crossfade on, compact on; to choose after trying them in play.

**The original proposal:** 0.25 m blocks everywhere in the valley (it was 0.25 m on the floor, by the mouth and along the river, 1 m on the mountains), drawn coarser with distance, rebuilt on worker threads, skirts between sizes, collision fine only near the player. Later, editor tools to shape the land by hand (river and trail points, ground painting, raise and lower) would fit on top.

## NPCs and hauling (branch `features/hauling`)

- **NPC milestones 1–4** (commit `1f803af`) and the NPC tools (`234923b`): see `NPCs.md` §10.
- **Hauling H1–H12** (2026-11-23, not committed): see `Hauling.md` ("Built").
