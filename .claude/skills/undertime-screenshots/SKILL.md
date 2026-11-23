---
name: undertime-screenshots
description: Take screenshots of UNDERTIME (the voxel mining game) to check visuals without a person at the keyboard - launch the game at a chosen spot, time, weather or the showcase cave, capture the window, and read the log. Use when asked to check, compare or tune how something in UNDERTIME looks, or to show the user a result.
---

# Screenshots of UNDERTIME

`Undertime/Tools/Screenshot.ps1` launches the built game straight into play at a given spot,
waits, captures the game window to a PNG, closes the game and prints the log lines that matter.
Look at the PNG with the Read tool; send it to the user with SendUserFile.

## Steps

1. **Build first.** Use the Release configuration, unless the user has the Release editor open
   (then it locks `CherryEngine.dll`: use Debug and `-Config Debug`).
   `cmake --build out/build/x64-debug --config Release` (the cmake that ships with Visual Studio,
   see CLAUDE.md). Shader changes in `CherryEngine/Shaders` reach `bin/` only with a full build,
   not `--target UndertimeGame`. Content shaders and materials (`Undertime/Content/...`) are
   read from the source tree, so editing them needs no build.
2. **Run the script** from PowerShell (one call per shot; each launch takes 10-15 s):
   ```powershell
   & Undertime\Tools\Screenshot.ps1 -View "24.5 1.6 14 180 0" -Out shots\mouth.png
   & Undertime\Tools\Screenshot.ps1 -Showcase -View "30 1 14 90 8" -Out shots\ores.png -Time "12 3"
   & Undertime\Tools\Screenshot.ps1 -View "-75 9 -133 70 -2 hold" -Out shots\rain.png -Time "11 3" -Weather "1 1 1"
   ```
   Put shots in the session's scratchpad, not the repo.
3. **Read the PNG** and judge it. Compare before and after by shooting the same `-View`/`-Time`/
   `-Seed` again. Several shots side by side: paste them into one image with Python's PIL.

## Parameters

| Parameter | Meaning |
|---|---|
| `-View "x y z yaw pitch [hold]"` | feet position in metres; yaw 0 faces +z (into the mountain), 90 faces +x; pitch > 0 looks down; `hold` keeps the player in place (mid-air views) |
| `-Time "hour [years]"` | hour of the outside day, optionally years after arriving (and that era: 0.5 Camp, 3 Settlement, 15 Boomtown, 40 Industry, 70 Abandoned, 110 Gone) |
| `-Weather "overcast rain wet"` | 0..1 each; omit for the clock's weather |
| `-Seed N` | always the same cave (the cave is random every launch otherwise) |
| `-Showcase` | the showcase cave: a fixed test hall just inside the entrance |
| `-Sun "x y z"` | override the direction sunlight travels |
| `-Talk "id [continues]"` | stand in front of a person (agnes, tom, mae, bram, clockmaker) and open a conversation, stepping past that many lines (1: the topic list); `-View` is still required but ignored |
| `-Missions "ids... [journal\|board]"` | take on missions (agnes_roof, tom_pan, mae_supper, mae_list, bram_cache, clock_year) and open the journal or the notice board (stand at `"-10.1 1.6 -127.5 0 6"` to see the board) |
| `-Wait s` | seconds before the capture (default 10; the cave's eye adaptation needs ~10) |

## Where to point the camera

- **The cave:** use `-Showcase`. Its hall (x 12..36, floor y 1, z 6..28) holds every ore, the
  Time crystal, cracked stone and gold, a crystal cluster, a lamp and the zone-1 shaft's light.
  The log prints a ready-made `UNDERTIME_DEV_VIEW` for each spot: hall, ores, cracks, crystal,
  shaft, lamp. Don't hunt through random caves: a guessed spot usually lands inside rock (the
  picture is then all black, or sky through the rock).
- **The mouth:** outside `"24 3 -12 0 5 hold"`, inside looking out `"24.5 1.6 14 180 0"`.
- **The town:** `"-75 9 -133 70 -2 hold"` (yaw 250 faces the afternoon sun);
  street level `"-20 3 -125 200 10 hold"`.
- **Water:** the river `"20 6 -200 228 15 hold"`, the north-east waterfall
  `"172 13 -12 40 25 hold"`, from afar `"150 10 -70 22 -8 hold"`, the lake `"74 25 -270 180 30 hold"`.
- **High over the valley:** `"0 40 -100 200 12 hold"`.
- The log also prints each natural shaft's foot ("shaft into zone N, foot at ...").

## Gotchas

- Settings go to the game as `UNDERTIME_DEV_*` environment variables. The script sets them for
  one launch only. If you set them by hand, set them in the same shell call as the launch: shell
  state doesn't carry between tool calls.
- The game can't be driven by key presses from a script (sending keys is unreliable). Use the
  parameters instead: `-Time` instead of T, the showcase instead of F9, and so on.
- Close any running game first, or a build that relinks the engine fails (LNK1104/LNK1168).
- In the valley at 17:00 and later, the town is in the mountains' shadow; mornings and early
  afternoons show sunlight and shadows best.
- A shader that fails to compile shows a magenta checker. The error is in the `.log.err` file
  next to the PNG (the script prints the first lines).
- Debug-only checks you can't script (motion, timing, flicker) need the user: say what to try in play.
