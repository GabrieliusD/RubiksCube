# Dice (CLAUDE.md for Dice/)

Guidance for working on Dice. The engine, the build and the branch rules are in the root `CLAUDE.md`.

## What it is

A port of a dice roll feel prototype. The Three.js original is in `C:\Users\gabri\DiceDemo`, with its spec in `docs\PROTOTYPE_EXPORT.md` (Part C, the reference code, wins over Part B). The plan, decisions, approved engine additions and milestone status are in `Docs/Port.md`. Dice is the one game that also builds for Android (`Docs/Android.md`).

## Architecture

- **Simulation:** pure, in `DiceSim` (a struct of arrays, no entity per die). It keeps the reference's right-handed coordinates and three.js quaternion maths (`DiceMath.h`); everything drawn is mirrored once, z -> -z (`ToEngine` in `DiceVisuals.h`).
- **Drawing:** every die goes through `Renderer::MeshInstances` (one instanced draw).
- **Tuning:** `DiceConfig.h` holds every setting and default from the reference. `Content/Presets/*.json` are named setting sets.
- **HUD:** an RmlUi document (`Content/UI/Hud.rml` + `Hud.rcss`, filled by `DiceHud.cpp`).
- **Upgrades (not in the reference):** coins banked from session scores buy levels in a tree (`Content/Upgrades.json`, rules in `DiceUpgrades.h`, screen `Content/UI/Upgrades.rml` + `DiceUpgradesUi.cpp`, progress in `Saved/Progress.json`). Levels set luck, density, session length and die types (golden: x score, lucky: + luck).
- **Audio:** `DiceAudio.cpp` (AAudio on Android).
- **UI effects gallery:** `DICE_DEV_GALLERY=1` shows a tile of each RmlUi effect (`Content/UI/Gallery.rml`, also parsed by `DiceTests`).

## Tests and tools

- `DiceTests` (CTest) covers the simulation and the UI documents. `DiceBench` is a benchmark, not registered with CTest.
- `Tools/Screenshot.ps1` runs the game with development switches (face, density, pointer, preset, session, coins and levels, gallery, editor, window size) and captures the window to a PNG. Its header lists every switch.
- Android: `Tools/Android/Build.ps1` and `Tools/Android/Package.ps1 -Project Dice/Dice.cherryproject` at the repository root (see the root `CLAUDE.md`).

## Branches

Dice work happens on `dice/<topic>` branches (see "Branches and commits" in the root `CLAUDE.md`).
