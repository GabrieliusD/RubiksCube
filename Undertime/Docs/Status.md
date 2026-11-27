# UNDERTIME status (2026-11-24)

What's built, what the user has tested, and where it lives. Decisions are in `Design.md`; how each piece was built is in `Changelog.md`.

## Branches and commits

Everything is on `master` (pushed; tagged `v0.1-pre-split` on 2026-11-27, when the old `features/*` branches were deleted). New UNDERTIME work goes on `undertime/<topic>` branches.

| What | Commits (on `master`) |
|---|---|
| milestones 1–15 (were on `features/voxel-town`) | 1–4 `813c198`, 5–6 `328a135`, 7–12 `e8bf6d6`, then ore seams `553f448`, rock relief `97be0e4`, valley LOD `c6a8593` |
| NPC milestones 1–4 and tools (were on `features/npcs`) | `1f803af`, `234923b` |
| hauling H1–H12 (was on `features/hauling`) | `814eb89` (committed together with the Dice port, as "Dice Demo") |

## Built

| Area | What | Tested by the user |
|---|---|---|
| The cave game | mining, cave-ins, boulders, loose blocks (none ever lost), shovel, backpack | yes (loose blocks: first test 2026-11-22, then fixed) |
| Voxel model pipeline, engine visuals I and II, the valley, Camp and Settlement eras, fixes | milestones 1–7 | yes (2026-11-21) |
| The outside clock, day/night, HUD watch, eras from the clock | milestone 8 | no |
| Light and depth: daylight in the cave, natural shafts, SSAO, cloud shadows | milestone 9 | no |
| The mountain matches its face | milestone 10 | no |
| Houses: interiors, glass, indoor light | milestone 11 | no |
| Particles | milestone 12 | no |
| Light shafts, foliage, era grade, eye adaptation, water, edge highlights | milestone 13 | partly (water and shadows looked at three times) |
| Valley haze, mining feel, the cave comes alive, weather; the showcase cave; rock relief | milestone 14 and after | partly (showcase cave looked at 2026-11-23) |
| Valley LOD, streamed collision, crags | milestone 15 | partly (pop-in noticed; options to compare) |
| People in the valley, talking, friendship, the save game | NPC milestones 1–2 | no |
| Missions, notice board, journal; lives across the eras, letters, graves | NPC milestones 3–4 | no |
| People inspector, Dialog view | NPC milestone 5 (tools only) | no |
| Money, shops, the yard, the barrow, better picks, the claim and hammer, the bench, hunger, wood, farming, machines, rails, town projects | hauling H1–H12 | no |

## Not built

- **Saving the mined cave** (the grid, loose blocks and piles). A load rebuilds the mountain unmined, so props and drills in the cave stand in rock. See Design.md §3 "Saving".
- Turning time back at the clockmaker.
- Rent, debt, the bank and interest (Design.md §2), price drift.
- The watches' story, the heart and the ending.
- Danger (cave-in damage, being trapped, the lantern running out).
- The Boomtown, Industry, Abandoned and Gone eras' town content.
- Seasons.
- Animals; places of interest.
- Hand-made models and written stories for the cast.
- Doors that open, upstairs floors, house lamps as real lights, soft particles, reflections in water at night.

## Next

A playtest pass over everything untested (Design.md §10 has the questions), then saving the mined cave, then the rewind rules (Design.md §3), in an order the user picks.
