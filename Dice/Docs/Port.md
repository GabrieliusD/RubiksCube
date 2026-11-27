# Dice roll feel prototype: the CherryEngine port

The source is `C:\Users\gabri\DiceDemo\docs\PROTOTYPE_EXPORT.md` (Part B spec, Part C
reference code; Part C wins). The port goes milestone by milestone (B14), checking each one's
"done when" before the next.

## Decisions

- **Coordinates (B13.6).** The simulation keeps the reference's right-handed coordinates and
  three.js quaternion maths (`DiceMath.h`, double maths, float storage). Everything drawn is
  mirrored once, z -> -z (`DiceVisuals.h`, `ToEngine`): meshes, instance matrices, camera, sun,
  and the mouse ray coming back. The left-handed engine then shows the reference's picture
  exactly, and no formula needed a sign change. Tested: the tumble rolls the die forward over
  its leading edge in every direction, and the mirrored die matrix equals the reference's.
- **No per-die entities.** `DiceSim` is a struct of arrays. Each frame the module writes one
  `MeshInstance` per die (`Renderer::MeshInstances`), with its world-space squash matrix and a
  colour tint. The engine draws them all as one instanced draw.
- **Lights.** three.js r160 physical lights divide by pi where the engine's strengths include
  it. So the sun is 2/pi. The hemisphere light becomes an ambient of 1.1/pi from above with
  `ambientContrast` 0.85 (#4a5262 is about 15% of white). Fill lights, sky, fog, bloom and SSAO
  are off. Shadows are fitted to the board (`ShadowSettings::boundsExtents`).
- **Config.** `DiceConfig.h` holds every B12 name and default (the strings became enums).

## Engine additions (approved by the user, 2026-11-24)

- `Renderer::MeshInstances()`: meshes drawn this frame without entities. Each has a mesh,
  material, matrix, tint and castShadows, and they go through the normal culled, instanced,
  shadow-casting path. `InstanceData::tint` multiplies the vertex colour in `MeshPass.hlsli`;
  it is 1 for entities.
- `Camera::orthographic` / `orthoHalfHeight`. The parts that change with it:
  - `ScreenPointToRay` starts each ray on the near plane;
  - view culling uses a box;
  - the shadow cascades fit an orthographic slice.
- `ShadowSettings::boundsCenter` / `boundsExtents`: fit both cascades to a fixed world box.
- `RenderSettings::toneCurve` (default on): off, the tonemap only clips at 1 instead of applying
  the ACES curve (`TonemapConstants::toneCurve`).

## Status

| # | Milestone | State |
|---|---|---|
| M1 | View: camera, lights, board, die mesh and atlas, instanced square grid of 1,600 dice, FPS | Done. `DiceTests`: face-up mapping, atlas pip counts per face, faceUp[f] shows f pips on top, tumble direction, mirror, board deal. Screenshots: shadows, six-up check (`Tools/Screenshot.ps1 -Board 6 -Face 6 -Density 100`). |
| M2 | Slots, spatial hash, mouse-to-plane, hover ring, READY / COOLDOWN dim and glow | Done. `DiceTests`: nearestSlot = brute force (4,000 points, on and off the board), freeOnly always free; exactly the READY dice under the ring respond (300 rings, radius 0.3..6); hovered slot; dim 0.6 for the cooldown, then the glow. Screenshots with `-Pointer`: the ring is centred on the pointer pixel; a sweep leaves a dimmed trail that glows when ready. Since M3 a hovered die launches. |
| M3 | Launch with reflection, pickFace, nudge, path / arc / tumble / squash, exact landing | Done (feel still to be judged by hand). `DiceSim::Frame` runs B6's order, so tests drive the same loop as the game. `DiceTests`: B11 invariants 1–4 every frame, 6 (face, end point and rotation exact) on every landing, and 5 (all landed 2.5 s after the pointer leaves). That's 9 setups × 2 seeds (default, full, 6×6 full, 80×80, 30 fps, 144 fps with 0.1 s spikes, long and short rolls, tumble 0 and 3, no cooldown with a 6-cell ring), 450–1,700 landings each. Also: 7 (luck 0 fair; luck 1 mean matches, sixes ~400× ones), no jump at launch or landing, never below the board, and a B13.1 re-entry flag that must stay clear. Screenshots: dice caught mid-tumble with their shadows. Collision handling here is nudge only; clash check, push, bounce and stack are marked `[M5]`/`[M6]`/`[M8]` in `Launch`/`ResolveTarget`. |
| M4 | Session timer and phases, scoring, popups, audio | Done (sound not heard yet). `DiceSim`: phases free → running → finishing → ended, score and session stats, bump tokens, the event queue with its bump / clash / topple effects (fired from M5 on), and an outbox of popups, sparks and sounds for the module. `DiceTests`: free play never scores and its popups are grey; Start gives 30.0 s; hovering launches nothing while finishing; late landings count; the score equals the counted landings; `sessionAirborne` matches the session's dice in the air every frame; it ends 0.5 s after time is up; a Restart mid-session leaves the old dice as free play; synthesised sound lengths and decays. Screenshots (`-Session S`): HUD, the yellow ring while running, popups, the end card. |
| M5 | Clash prediction + per-frame separation and glancing | Done. `CheckClash` at launch (B10.6), `ResolveMidAir` after the rolls (B10.7, with each die's bucket cell stored, B13.8), `Glance` (B10.8). Sparks: a pool of 600 simulated in the module, drawn as square additive engine particles 5 px across at any zoom. `DiceTests` on a crowded 30×30 board (ring 4, cooldown 0.2, 3 seeds): airborne pairs whose centres are under half a die apart (and overlapping in height) — 0 with clash + glancing on, 3,636 pair-frames with both off; the closest pair stays 0.34 apart (vs 0.003); 901 clashes; invariants hold and everything lands. The 18 M3 scenario runs now have clashes on as well. Screenshots: CLASH +25 popups and sparks. |
| M6 | Bump: push at contact, bounce-off, wobble, chain limit and tokens | Done. `ContactU`, the push and bounce-off branches of `ResolveTarget` (B10.1–B10.3); the wobble was already in the render (M1). The panel has the B12 collision controls (except stacking) and push / bounce / clash counts. `DiceTests`: every M3 setup again in bump mode, plus always push with chains to 10 and 200 tokens/s, always bounce, 1 token/s, a full 6×6 board with chains to 10 and no cooldown, and pushes with clashes and glancing off. Each holds the invariants every frame, with no duplicates, chains ≤ `bumpChainLimit` and tokens ≥ 0. Single rolls (300): a push spends a token, takes the slot and starts the occupant at the moment of contact (bumpContact × dieSize apart) in the hit's direction; a bounce leaves the occupant (wobbling at the hit), turns at the contact point at that moment and lands in a free slot. Screenshots (`-Mode 1`): blue half-score push popups among clashes. Test note: B11.5's "about 2 s" assumes default rolls; the settle time scales with the longest push chain (1.5 s rolls can take ~4 s). |
| M7 | Hex, scatter, shapes, slot markers, rebuild | Done. `GenHex`, `GenScatter` (Bridson), `InShape` (B4). Boards other than the classic square/full get plain felt plus one instanced marker per slot (three.js `CircleGeometry` shapes). Panel: layout, shape and scatter spacing (rebuild); `-Layout` / `-Shape` for screenshots. `DiceTests`: all 24 combinations (3 layouts × 4 shapes × nudge / bump, 2 seeds each) hold the invariants and land everything, with clashes, pushes and bounces in each. Geometry: square N²; hex offset rows with every nearest neighbour exactly 1 apart; scatter at 0.7 / 0.95 / 2.0 never closer than the spacing, inside the margin and with no gap over 2× spacing; every shape filters its slots. 80×80 hex and densest scatter fit the capacity (see "Known differences"), and a rebuild mid-roll clears the active list, events and effects. Screenshots: hex full, scatter circle, square ring, hex islands with their markers. |
| M8 | Stacking: stackOn, topple, top-only hover, uncover cooldown, stack scoring | Done, with one fix to the reference (see "Known differences"). `StackOn`, `Topple` and the stack branch of `ResolveTarget` (B10.4, B10.5). Top-only hover, the uncover cooldown and stack scoring were already in from M2–M4. The panel has stack mode, max height, chance and stack / topple counts. `DiceTests`: packed boards (density 1 and 0.9, stackMax 2–6, a pointer hopping about for 20 s so tall stacks form and get hit) and the default settings hold the invariants every frame, with 258 topples; topples happen at stackMax 2, 3 and 4 (tops get relaunched before stacks fill above that). All 12 layout × shape combinations hold the invariants in stack mode too. Single rolls (100): land on top at level 1 and height dieSize; "+N ×1.5" scoring; a topple knocks the upper die off at contact and settles on the bottom one, with "TOPPLE ×1 +15" counting in the session; hovering a stack launches only its top; the uncovered die gets a fresh cooldown. Screenshot: stacks and ×1.5 / ×2.0 popups. |
| M9 | Debug panel + presets | Done (the widgets themselves not yet clicked by hand). `DiceControls`: every B12 tunable by name (`Fields`), config ↔ JSON, and the panel table `Controls()` — the reference's CONTROLS list, from which `DicePanel.cpp` draws the panel as the reference does: label with the value right-aligned, sliders snapped to their HTML step, board-rebuilding controls rebuild. Presets: `Content/Presets/*.json` (each lists only what it changes from the defaults; shipped: bump, stacks, hex-islands, scatter-ring, big-lucky, chaos), Load / Save / Defaults in the panel, `DICE_DEV_PRESET` / `-Preset` at start-up. Hot reload keeps the config (`SaveState` JSON). `DiceTests`: the config is exactly B12's 56 names in order with B12's defaults; every field survives JSON; partial presets change only their keys, and unknown keys or bad values are reported, not applied; the panel table equals the reference's 36 controls (keys, kinds, ranges, steps, rebuilds); snapping; all shipped presets load cleanly; 360 random control changes mid-roll (every control at least 3 times, 53 rebuilds) keep the invariants. |
| M10 | Profile at 10k / 50k / 100k dice | Done: 60 fps at 10k with large headroom; findings below. |

### Upgrades (not in the reference)

A progression layer on top of the prototype, on by default (the debug panel's "Upgrades" section
switches it off, back to the prototype's luck, density and session length; the prototype's dev
switches, density, preset and profile, also start with it off).

- **Coins:** a finished session banks `score x coinsPerPoint` (0.05) coins (`BankSessionCoins`); the HUD
  shows them, the end card shows what was earned. Saved in `Saved/Progress.json`.
- **The tree:** `Content/Upgrades.json` (format in `DiceUpgrades.h`): nodes with levels, a price
  (`cost x costGrowth^level`), requirements on other nodes' levels and effects on stats: luck,
  density (the start is 0.3, so "More dice" means something), session seconds, golden and lucky
  dice shares, the golden multiplier and the lucky luck. Shipped: More dice, Longer rounds, Golden
  dice, Midas touch, Loaded dice, Lucky dice, Four-leaf clover.
- **Die types** (`DiceSim::kinds`, `D.kind`): dealt by share when the board is dealt (no extra
  random numbers when every share is 0, so the reference's deal is unchanged); golden dice score
  `goldMultiplier` times (gold popups), lucky dice roll with `luckyLuck` added to the luck; tinted
  gold and green on top of the glow and dimming.
- **The screen:** `Content/UI/Upgrades.rml` (U, or the HUD's and end card's Upgrades buttons; not during
  a session): the tree laid out from the JSON, links lit once met, buyable nodes glowing, a detail
  line with what the next level changes. Buying More dice or a die type deals the board again.
- **Checks:** `DiceTests` "upgrades" and "die types" (prices, states, buying, stats, the save, tree
  errors, golden scoring, lucky faces, shares) and the screen loading headless. Dev:
  `DICE_DEV_COINS`, `DICE_DEV_LEVELS`, `DICE_DEV_UPGRADES`, `DICE_DEV_PROGRESSION`
  (`Screenshot.ps1 -Coins -Levels -Upgrades`).
- **Not in the reference either:** the HUD's buttons brighten on hover.

### Known differences

- **Hover ring** is drawn with ImGui (the board circle projected to the screen, over the 3D
  view), so ImGui antialiases it rather than FXAA. The cursor is hidden over the view, as in the reference.

- **Tone.** The reference has no tone mapping, so the module turns the engine's ACES curve off
  (`RenderSettings::toneCurve = false`). Measured against the reference after that (sRGB): tops
  232 vs 232, shaded sides 112 vs 112, felt (56,88,80) vs (48,88,80).
- **Ground ambient** has one share for all channels, so it's a touch less blue than #4a5262.
- **UI** (the HUD, FPS box, end card and score popups) is an RmlUi document, `Content/UI/Hud.rml`
  and `Hud.rcss`: the reference's markup and stylesheet ported almost line for line (sizes in
  `dp`, so they follow the display's DPI as CSS pixels do; RCSS differences are listed at the
  top of `Hud.rcss`). `DiceHud.cpp` fills its "dice" data model; Start / Restart call its
  "start" event. The font is Segoe UI (the reference's `system-ui` on Windows; Roboto on
  Android), registered as `system-ui`. Segoe has no weight 800, so bold (700) stands in for it;
  semibold covers 600 and Black covers 900. The text shadows are glow font effects. The popups
  are a fixed pool of 140 elements made and moved from code, as the reference's script does,
  each popup keeping its element for its life. Binding them through the data model instead
  (a transform string per popup per frame) cost 1.7-2 ms of RmlUi update a frame with the pool
  full; setting the transform as numbers (`Rml::Transform::MakeProperty`) brought a full pool
  to about 0.4 ms in all (Release, frame 2.7 ms -> 1.1 ms; 0.7 ms with no popups). The hover
  ring stays an ImGui overlay, and the debug panel stays ImGui. `DiceTests` loads the document
  headless and checks it gives no warnings. `DICE_DEV_GALLERY=1` (`Screenshot.ps1 -Gallery`)
  adds `Content/UI/Gallery.rml`, a tile of each UI effect the engine draws, to check them.
- **Audio** is XAudio2 in the module (the engine has none). The sounds use the reference's
  WebAudio synthesis, rendered once into buffers; knocks and ticks come in 8 variants at the
  reference's random band-pass frequencies. It starts on the first click or on Start.
- **Sparks** are engine particles, which are lit like the scene: their colour is scaled down
  and their emissive up (×20), so the light adds under 4%. Like the reference's points they're
  depth-tested and additive, and they're drawn the same 5 px at any zoom.
- **The hover ring** goes on ImGui's background list in the full-window game (over the 3D view,
  under the panel) and on its foreground list in the editor viewport. The RmlUi HUD is drawn
  into the view itself, so the ring is over it; the mouse over the HUD belongs to the HUD,
  which hides the ring there.
- **Capacity** is 80² × 1.3 = 8,320 slots, not the reference's 80² × 1.2 = 7,680. Scatter at
  its minimum spacing 0.7 makes about 8,100 slots on an 80×80 board. The reference silently drops
  the overflow, which leaves a hole where Bridson sampling would have grown last. Found by the M7
  capacity test.
- **Topple claims the slot first** (`DiceSim::Topple`). The reference launches the upper dice
  and only then calls `stackOn(i, bottom, target)`. A knocked-off die whose throw reflects off
  the board edge back onto the same slot (`reflect` maps a throw of exactly twice the distance
  to the edge back to its start) stacks on the bottom die again. The final `stackOn` then
  overwrites the slot's top, leaving that die in no chain: two dice on one spot, one of them
  never hoverable again. The port claims the slot first, as a push already does, so that die
  finds the slot taken and nudges. Found by the M8 invariant runs (stackMax 2, hopping pointer).
- **Presets** are new (B14 asks for them; the reference has none). Tunables outside the
  reference's panel (jitter, wobble, scores, session length, ...) are changed through presets.
  Lowering `stackMax` at runtime leaves taller stacks standing, as in the reference, so the tests
  allow chains up to the tallest `stackMax` since the last deal.
- **Shadow map** is 2048 over the board's light-space bounds (similar to the reference's ±0.8N box).

## Performance (M10, 2026-11-24)

Ryzen 9 5900X, RTX 4070, Release build, vsync off, full-window game view. Square boards at
density 1: 100×100 (10,000 dice), 224×224 (50,176), 317×317 (100,489). Each run uses bump mode,
a 6-cell ring sweeping the board, and optionally a storm (10% of the dice launched per second at
random, `DICE_DEV_STORM`). The numbers are averages over 2 s from `DICE_DEV_PROFILE` lines:

| Board | In the air | FPS | Frame | Sim | Instance writing | Rest of the frame (engine CPU) | GPU |
|---|---|---|---|---|---|---|---|
| 10k, sweep | ~60 | 370 | 2.7 ms | 0.02 ms | 0.13 ms | 2.5 ms | 0.6 ms |
| 10k, sweep + storm | ~550 | 340 | 2.9 ms | 0.16 ms | 0.15 ms | 2.6 ms | 0.6 ms |
| 50k, sweep + storm | ~2,600 | 59 | 16.9 ms | 3.0 ms | 0.8 ms | 13.1 ms | 2.6 ms |
| 100k, sweep | ~120 | 36 | 27.4 ms | 0.13 ms | 1.4 ms | 25.8 ms | 4.4 ms |
| 100k, sweep + storm | ~5,100 | 20 | 50.8 ms | 23.3 ms | 1.6 ms | 26.0 ms | 5.8 ms |

`DiceBench` (the simulation alone, at a fixed 1/60 s step) agrees: 10k costs 0.06–0.3 ms.
50k with ~3,000 in the air costs 2.6 ms; 100k with ~5,700 costs 9.4 ms, and with ~12,800 it
costs 48 ms.

Findings:
1. **10k is comfortably real-time:** ~2.9 ms a frame, about 5.7× under the 16.7 ms budget.
2. **Above ~30k, the engine's per-instance submission is the main cost** (~0.26 µs per
   instance), not the GPU (a few ms). `Renderer::MeshInstances` goes through the generic entity
   path. Per instance, that path:
   - transforms the mesh bounds;
   - tests both shadow cascades and the view;
   - inverts the matrix for the normal matrix;
   - sorts every draw item.

   Then it uploads 160 bytes an instance (16 MB a frame at 100k). The likely fix is an engine
   addition: a pre-sorted batch (one mesh and material, no per-instance culling or sorting, the
   normal matrix derived in the shader), then B16's "only update the instances that changed".
3. **The simulation's cost grows with dice in the air × launches per frame.** The clash check
   at launch (B10.6) scans every airborne die, the same as the reference. It's negligible at
   10k (0.16 ms), but at 100k with ~5,000 in the air it's 23 ms. It also feeds on itself: a
   slower frame clamps to dt 0.05, which means more launches per frame. A spatial grid of the
   airborne dice's paths would cut it without changing results (the pruning only has to be
   conservative).
4. **Writing the instance matrices is linear and small:** 1.4–1.6 ms at 100k.

## Running

- `out/build/x64-debug/bin/Debug/CherryLaunch.exe Dice/Dice.cherryproject --play`
- Tests: `ctest --test-dir out/build/x64-debug -C Debug -R DiceTests`
- Simulation benchmark: `out/build/x64-debug/bin/Release/DiceBench.exe`
- Profiling the game: `powershell -File Dice\Tools\Screenshot.ps1 -Config Release -Out shots\p.png -Board 317 -Density 100 -Storm 10 -Profile -Pointer "300 450 900 300 4" -Wait 11` (the `DICE_PROFILE` lines are in the log)
- Screenshots: `powershell -File Dice\Tools\Screenshot.ps1 -Out shots\x.png [-Board N] [-Face 1..6] [-Density P] [-Seed N] [-Pointer "px py [px2 py2 seconds]"]`
