# UNDERTIME: the NPC system (proposal, 2026-11-23)

**Status: a proposal, with the user's first decisions (below).** Everything not listed under "Decided" is still a proposal. Decisions are also recorded in `Design.md` §5.

What the user asked for: NPCs that are simulated, have personality, talk with the player, give missions, have a friendliness level, and whose behaviour keeps being simulated as time moves.

### Decided (2026-11-23, the user's decisions)
- **No randomly generated NPCs.** Every NPC is written by hand, with their own story. The town's population in each era is the written cast.
- **Placeholders for now.** NPCs start as placeholders (looks and stories) and will need much more detail later.
- **Rewards can be earned again.** A mission replayed after turning time back pays its reward again (for now).
- **Time pauses during a conversation.**
- **Friendships fade** while you're away.
- **The clockmaker remembers you** across rewinds, the only one who does (§4).
- **The first cast** (placeholders): the Camp era's trading post keeper, the clockmaker and two or three campers, then the Settlement shopkeepers.
- **The notice board stays,** with generated jobs, each posted by a written NPC (§6).
- **A save game** is built as part of NPC milestone 2 (§9).
- **The final look: hand-made models** in MagicaVoxel, one per character (§8).
- **Also in, from the suggestions:** déjà vu for NPCs from a timeline you undid (§4), time-lapse figures seen from the mine while time races (§2.4), letters while you're underground (§7), and weddings, funerals and festivals on fixed dates (§7).

---

## 0. In short

1. **An NPC is a function of the outside clock.** Their state is computed from the date plus a log of what the player did. They aren't stepped frame by frame. That's the only approach that survives a zone 5 dive (a year a minute) and turning time back (decided, Design.md §3).
2. **Three time scales:** a *life* (years: arriving, marrying, children, the shop changing hands, dying), a *day* (hours: a schedule of places and activities), and the *moment* (seconds: walking, looking at you, talking, only near the player).
3. **Personality** is five trait sliders plus likes and dislikes. They drive what NPCs say, how their regard for you moves, their routine and the missions they give.
4. **Friendliness (regard)** rises through talking, gifts, missions and trade, and **fades while you're gone**. A deep dive costs you friends, which is the game's theme applied to people. Rewinding past the day you met someone makes them forget you (already decided).
5. **Missions have deadlines in outside time**, so choosing a zone is also choosing which promises you can keep. Some missions *need* time to pass ("come back in five years").

---

## 1. The constraint that shapes everything: time

| Where you are | 1 real minute = | What that means for NPCs |
|---|---|---|
| Surface | ~1.6 hours | You can watch them live: walk, work, talk |
| Zone 1 | 8 hours | A third of their day per minute |
| Zone 3 | 1 week | A whole week of routines per minute |
| Zone 5 | 1 year | Births, marriages and deaths every few minutes |
| Turning time back | years backwards | Their lives have to "un-happen" exactly |

A per-frame simulation (Sims-style needs, random decisions) breaks here. A year of it would have to run in a minute, and a rewind would need to restore an old copy of the whole world. So:

> **An NPC's state at any moment = f(NPC definition, outside date, the player's log).**

- **Deterministic**, like `WeatherAt(days)` already is: the same day always gives the same result. Rewinding replays the same lives, the same weather and the same moods.
- **Any jump is free:** surfacing after 40 years just evaluates everyone at the new date.
- **Rewinding is simple:** drop every log entry after the target date and re-evaluate.
- **Testable:** pure C++ in `UndertimeTests`, like `Clock.h` and `DayNight.h`.
- **Saving is small:** the save holds the clock, the log and the mission states. Everything else can be rebuilt from them.

### The player log (the only mutable NPC state)
An append-only list of timestamped events (outside days):

```
{ day: 412.3, npc: "mara",   kind: "talked" }
{ day: 415.0, npc: "mara",   kind: "gift",     item: "copper", count: 5 }
{ day: 415.0, npc: "mara",   kind: "mission",  id: "mara_ring", state: "accepted" }
{ day: 900.7, npc: "doctor", kind: "lifeHook", hook: "medicine_delivered" }
```

Regard, mission progress, "last seen", flags and changes to someone's life (see §6) are all folded from this list. Winding back to day D removes entries after D.

---

## 2. The three time scales

### 2.1 Life (years)
Every named NPC has a **life script**: a short list of dated facts and branches.

```
born -22y, arrives 0.1y (Camp), role "trapper"
3y:  role "trading post keeper"
8y:  marries "tom"; 10y child "ella" (inherits traits)
25y: hands the shop to "ella"; role "retired"
41y: dies (graveyard) unless lifeHook "medicine_delivered" -> dies 58y
```

- **Every NPC is written by hand** (decided): the widow, the family, the doctor, the teacher, the boatman, the shopkeepers, the clockmaker, and everyone who arrives later. There are no generated townsfolk. The town grows in Boomtown and thins out when Abandoned because written characters arrive and leave, so a busier era means writing more people. Their stories across the eras are still open in Design.md §5; for now they're placeholders.
- **Not everyone needs the same depth:** a main character has a full life script and many topics, while a minor one (the Boomtown barber, a company clerk) can have a short life, a schedule and a few lines. But each one is a person with a name and a story, never a template.
- **Descendants** are written too: their traits are authored (ideally echoing their parents'), and they start with a share of their family's regard for you (§4).
- **Roles move with the eras:** a shop is a role, not a person. The general store is run by Mara, then her daughter, then a stranger from the company, then nobody.

### 2.2 Day (hours)
A **schedule**: blocks of activity at places, chosen by hour, weekday, season, weather and era.

```
06-07 home: wake      07-12 store: work    12-13 saloon: eat
13-18 store: work     18-20 square: chat   20-06 home: sleep
rain: no "square", stays at "saloon"
Sunday: 09-11 chapel
```

- **Places** are named spots: building doors and their furniture (the counter, the bed, the forge; `Inside` kinds in `Tools/Placeholders.cpp`), plus points of interest (the lake dock, the lookout).
- **Where an NPC is at hour h:** the current block's place, or a point along the route between two places during the transition minutes. This is computed, never stored.
- **A daily mood:** f(day, weather, personality, recent life events, recent player events). For example, a grey rain day and a cave-in rumble the night before make the superstitious ones short-tempered. Mood shifts greetings and how much regard a conversation gives.

### 2.3 The moment (seconds)
This layer only exists near the player, while time runs at the surface rate:

- walking the route at a believable speed (a route that's late catches up quietly, or the NPC appears from a doorway);
- turning their head to you, stepping aside, waving if they know you;
- barks when you pass, conversations when you talk to them;
- small reactions: a cave-in rumble, rain starting, you sprinting through their garden.

The moment layer is presentation only. It never changes state, except by writing to the player log.

### 2.4 Level of detail

| Distance / situation | What runs |
|---|---|
| Within ~40 m, surface rate | Full puppet: animation, look-at, barks, talking, a kinematic blocker so you can't walk through them |
| 40 to ~200 m | Moves along its route, simple walk cycle, no barks |
| Beyond that, indoors out of view, or you're in the cave | No entity at all; evaluated only when needed |
| Time racing and the valley in view (the entrance passage, `]` key) | **Time-lapse figures** (decided): figures drawn faint and dithered (`MeshRenderer::dither` exists), moving fast along their routes, like a long-exposure photograph. You see the town's life racing past from the mouth of the mine. They fade in as the outside rate climbs past a threshold and are full figures again at the surface rate. Above a higher rate (when a person would cross the valley in under a second) they thin into faint trails along the busiest routes, then vanish. Thresholds in `Config.h`. |

---

## 3. Personality

Five sliders (0..1) plus tastes. Small enough to author and to read in dialog.

| Trait | Low end / high end | What it drives |
|---|---|---|
| **Warmth** | cold / warm | How fast regard grows; greetings; whether they invite you in |
| **Greed** | content / greedy | Prices at their shop; whether they like your mining (greedy ones love it); mission rewards; how they do in Boomtown |
| **Patience** | restless / patient | **How fast they forget you while you're away**; mission deadlines (impatient ones give short ones) |
| **Superstition** | sceptic / believer | What they think of the mountain and the deal; the founders' secret; hints about Strange watches; fear of cave-ins |
| **Curiosity** | incurious / curious | Asking about the cave; trading rumours; liking gifts of strange finds (crystal, watch fragments) |

- **Tastes:** a liked and disliked list over items (ore types, stone, crystal, fish, flowers, letters). Gifts read these.
- **Voice:** a tag (terse, chatty, formal, rough) that picks which version of a shared bark is used, so placeholder characters sound different from each other before their own lines are written. Written characters get their own barks.
- **Ageing:** traits drift a little per era and with life events (a widow's warmth drops for a few years, a rich merchant's greed rises).
- **Ties:** each named NPC has a few relations (family, friend, rival) to other NPCs. These are used for gossip (§4) and dialog ("Don't trust the banker").

---

## 4. Friendliness (regard)

- **A value from -100 to 100 per NPC, shown as tiers**, never as a number: *Hostile, Wary, Stranger, Acquaintance, Friendly, Friend, Close*. A small icon in the dialog box and a note in the journal.
- **What raises it:** talking (a small amount, once per day), gifts (by taste), finished missions, trade at their shop, funding town projects, being there for their events (a wedding, a festival).
- **What lowers it:** failed or abandoned missions, disliked gifts, mining (only for the superstitious, and more after cave-ins they felt), missing important events.
- **Absence (the twist; decided, friendships fade):** while you're away, regard drifts back towards a baseline, at a rate set by their *patience* and how close you were. Close friends fade slowest. They remember when they last saw you, so greetings change: "Back already?", "Three winters, courier. We thought the mountain had you." (`OutsideClock::SpanText` already writes the span.)
- **Gossip:** a share of a big change reaches their ties a few days later. Help the doctor and the doctor's sister warms to you. Cheat the banker and the town hears about it. **Town standing** (the average) affects the general store's prices and what's on the notice board.
- **Generations:** a child starts with part of their parents' regard. "My grandmother spoke of a courier who never grew old." For a game about outliving everyone, this is the emotional payoff.
- **Rewinding** past the day you met someone: they don't know you (decided).
- **Déjà vu (decided):** the game remembers on the player's side who you knew in a timeline you undid, and the best tier you reached with them. Such an NPC opens with a few unique lines ("Have we met? You have a face I... no."), and their regard grows a little faster the second time (more for closer lost friendships). The player keeps the memory; the NPC only has a feeling. This record is never truncated by a rewind.
- **The clockmaker remembers (decided):** the one person immune to time. Their regard and memory are kept on the player's side like déjà vu, so a rewind never touches them. They can talk about timelines you undid ("You were friends with the widow, last time. Does she know you now?"), which gives the valley's mystery a voice.
- **What higher tiers unlock:** better prices, missions, personal topics, rumours and watch clues, invitations (dinner in their lit house at night), letters while you're underground, and their epitaph mentioning you in the graveyard.

---

## 5. Dialog

### Structure: topics, not one big tree
Each NPC has a pool of **topics**. When you talk to them, the game picks:

1. **A greeting**, chosen by situation in priority order: first meeting, déjà vu, long absence, an event today (a wedding, a funeral), weather, time of day, mood, tier.
2. **A list of topics** whose conditions pass, for example "the mine", "the founders", "work for me?", "about Ella", "rumours", "goodbye".
3. Each topic is a **short tree** of lines and player choices.

A small number of hand-written trees for story and missions, plus shared, tagged pools for small talk. That's how the writing stays manageable across six eras and generations.

### Data
`Content/Dialog/<npc>.dialog` (JSON), reloaded when the file changes, like other assets:

```json
{ "topic": "mara_ring", "when": { "era": [2,3], "tier": ">=Acquaintance", "mission": { "mara_ring": "none" } },
  "lines": [
    { "say": "I lost my mother's ring in the old tunnel. Zone one, past the first lamp." },
    { "choice": [
      { "text": "I'll look for it.", "do": [ { "mission": "mara_ring", "state": "accepted" } ] },
      { "text": "Not now.",          "do": [ { "regard": -1 } ] } ] } ] }
```

- **Conditions:** era, tier, flags, mission states, hour, season, weather, days since you last saw them, items in the backpack, their life stage, a déjà vu flag.
- **Effects:** regard, set a flag, start, complete or fail a mission, give or take items or money, a life hook (§6), open a shop screen.
- **Text substitution:** `{player}`, `{name}`, `{away}` (time since last seen), `{season}`, `{era}`, names from ties (`{tie:daughter}`).
- **Barks:** one-liners when you pass, from shared pools tagged by situation and voice ("Mind the rain", "Heard the mountain rumble last night").
- **Validation test:** `UndertimeTests` loads every `.dialog` and `.mission` file and checks that every condition key, effect, mission id and NPC id exists. A typo becomes a failing test instead of a silent bug.

### On screen
- Press **E** near an NPC: the camera eases to face them, and an ImGui dialog box appears at the bottom (drawn like the HUD in `Hud.h`), with a portrait, their name, tier and the lines.
- **Portrait:** a render of their voxel head through `Renderer::RenderPreview(mesh, material, size, yaw, pitch)`, which exists (it serves the shader graph preview). Whether it works well from game code at runtime needs checking. A fallback is a 2D portrait image per NPC, which Design.md §7 already mentions for shopkeepers.
- **The clock during dialog (decided):** the outside clock pauses while a conversation is open, as does your own time. NPCs nearby freeze in place or carry on their idle animation without moving along their routes.

### Later: a dialog editor
The editor already has imgui-node-editor (the shader graph editor). A node view of topics and choices would make writing much easier once there's real content. Until then, hand-written JSON plus the validation test is enough.

---

## 6. Missions

### Definition
`Content/Missions/<id>.mission` (JSON): giver, era window, required tier, objectives, **deadline in outside time**, rewards, and what happens on failure.

### Objective types, built on systems that already exist

| Type | Example | Built on |
|---|---|---|
| **Deliver** | "40 copper for the new roof" | backpack counts per block type |
| **Retrieve from the cave** | "My husband's pick, zone 2, the flooded room" | a placed object in `CaveGenerator`'s layout (deterministic per seed) |
| **Courier** | "Take this letter to the boatman" | the courier premise; NPC positions from schedules |
| **Be there** | "Meet me at the lake at dusk on the 3rd of June" | the outside clock and a place |
| **Wait** | "Plant this sapling. Come back in five years." | the outside clock (a time investment, Design.md §9) |
| **Town project** | "The chapel needs 2,000 stone" | loose blocks and carts (Design.md §3, "What stone is for") |
| **Ask around** | "Find out who's been in my shed" | dialog flags |

### Outside-time deadlines (the signature)
- "Before the first snow", "by Sunday", "while I'm still alive". The journal shows the deadline in outside time and **what that is underground**: "5 days left: 15 minutes on the surface, 5 minutes in zone 2, 40 seconds in zone 3."
- **Some missions need time to pass:** the cask that has to age 20 years, the sapling, the child who'll be old enough to apprentice. You dive deep on purpose to finish them.
- **That gives a decision:** the gold is in zone 3, but Mara needs the copper by Friday.

### Failing, and rewinding
- Most missions just expire, and the giver is disappointed (regard down).
- Some have **consequences**: the medicine you didn't bring in time and the patient dies (a grave appears). With turning time back, a failure is a second chance: rewinding costs crystal plus the friendships made since, and the player chooses whether it's worth it.
- **Rewards and rewinds (decided for now):** money and items stay (Design.md §3), and a mission replayed after a rewind **pays its reward again**. If that turns into a way to farm money, the fallback is paying each mission's money once per game (recorded on the player's side), while a replay still gives the regard and the story.

### Life hooks
Some missions change a life script: `medicine_delivered` moves the doctor's patient's death from year 41 to 58; funding the school keeps the teacher in the valley through Industry. These are log entries, so a rewind undoes them like everything else.

### Generated jobs
The saloon's notice board (already in Design.md §6) posts simple jobs from templates (deliver N of an ore, carry a letter, clear a fallen tree) chosen deterministically per outside week. The same week always shows the same jobs, so a rewind replays them. Paid in money, with a little town standing. Only the jobs are generated: each one is posted by a written NPC whose needs fit it (the smith wants coal, the boatman a letter carried), never by a made-up person. The board is decided (2026-11-23); the job templates and which NPCs post which jobs are written data, like everything else.

### The journal
A HUD page: active missions with their givers, deadlines and the zone translation, plus a family tree of the people you've known across the eras. With the graveyard, this is where you see how long you've been here.

---

## 7. "As time moves": what the player sees

- **On the surface:** the town lives its schedule: shops open, people walk the trails, gather in the square in the evening, go home and light their windows (which already happens through `Materials/Town.mat`).
- **Coming up from a dive:** the come-up summary (milestone 8) gains **town news**, computed by comparing the NPCs' state before and after: "Mara married the blacksmith. Old Tom has died. The Hendersons have left the valley. Ella runs the store now."
- **The graveyard** fills with names of people you knew, with epitaphs that mention you if you were close.
- **Eras** change the cast: more written characters arrive in Boomtown, the company men arrive in Industry, only the last shopkeeper is left when Abandoned, and in Gone only the clockmaker remains.

### Letters while you're underground (decided)
- **Who writes:** NPCs at Acquaintance or above write while you're away. Each letter is written by hand and has conditions like a dialog topic: the writer's tier, how long you've been gone, their life events (a birth, a wedding, a shop changing hands, illness), upcoming dated events (an invitation), and missions (a reminder as a deadline nears, disappointment after it passes).
- **When:** a letter has a date on the outside clock (a fixed delay after its trigger, plus a minimum gap between letters from the same person), so the same timeline always gives the same letters, and a rewind takes back letters dated after it.
- **Where:** at the post office (from Settlement; in Camp, the trading post keeps them). On coming up, the HUD says how many are waiting. A long dive leaves a pile, the last few increasingly worried, and eventually one from a son or daughter saying their parent has died.
- **Reading:** a letter screen in the journal, kept afterwards. Letters can start missions and invitations.
- **The courier's own job:** letters for other NPCs that pile up while you're gone are a source of courier jobs (§6, the notice board).

### Weddings, funerals and festivals on fixed dates (decided)
- **Events** are dated on the outside clock and placed somewhere: a wedding at the chapel, a funeral at the graveyard, the harvest festival in the square, the clock tower's first strike. Births, weddings and deaths come from life scripts; festivals repeat yearly (by day of year, only in the eras that hold them).
- **You hear about them in advance:** an invitation letter, a line in dialog, a poster on the notice board, so you can plan a dive to come up in time. The journal lists those you know about, with the zone translation ("in 9 days: 3 minutes in zone 2").
- **Being there:** the NPCs involved gather at the place for its hours (a schedule override), with a few event lines and barks, and lights or decorations props shown only that day. Attending raises regard with everyone involved, and missing a close friend's wedding or funeral lowers it. Missed events stay missed unless you turn time back.
- **Funerals** close a life: the grave appears after it, with its epitaph.

### What gets recorded
Letters, events and town news are all computed from the life scripts and the player log, so none of it needs saving beyond the log itself: whether you attended is a log entry ("attended wedding_mara_tom").

---

## 8. Looks and movement (technical)

There are no animated characters in the engine (Design.md §7). A proposal that needs **no engine change**:

- **Voxel puppets:** each NPC is a few `.vox` parts (head, torso, two arms, two legs, plus a hat or tool) as entities under one root. `Transform` is local to `Parent`, so the game animates the parts procedurally: walk swing, idle sway, breathing, head look-at, gestures while talking. Blocky, and in keeping with the style.
- **Placeholders first (decided):** the `UndertimePlaceholders` tool writes a simple model per NPC, like the trees: body, head, hair, hat and clothes in that NPC's colours (clothes by era: canvas and leather in Camp, painted colours in Boomtown, soot and wool in Industry). Children are smaller; the old stoop and go grey. Every NPC still gets their own model, since each one is a written character.
- **The final look: hand-made models (decided).** Each character is modelled by hand in MagicaVoxel, as one `.vox` per body part (`Content/Models/Npcs/<name>/Head.vox`, `Torso.vox`, ...), with variants per era or age where their look changes (`Torso_Boomtown.vox`, `Head_Old.vox`). A placeholder uses the same file names, so a hand-made model replaces it by overwriting the files, with no code change. The voxel size for people (at 0.25 m a person is only 7 blocks tall, too coarse for a face, so probably finer) is chosen when the first real model is made; `VoxelModel` already stores a voxel size.
- **To check before then:** whether `LoadVox` reads a MagicaVoxel file with several objects. If it does, a character could be one file with a named object per part, which is easier to model.
- **Drawing:** the engine's instancing batches repeated parts (hands, legs), and the cast is a few dozen people at most, so drawing them is cheap.
- **Walking:** there's no navmesh. Use a **waypoint graph** made from the trails (`cfg::kPathSegments`), plot gates, building doors and a few points inside each building by its furniture. Height comes from `Valley`'s ground height. No physics bodies: NPCs are kinematic; near the player they get a blocker so you bump into them instead of walking through.
- **Doors** are open doorways, so NPCs can walk in and out. Their lit lamp at night already exists.
- **In the cave (an idea):** other miners in zone 1 during Settlement and Boomtown, working the timbered tunnels, and gone in later eras with only their lamps left. It would need cave waypoints.

---

## 9. Code layout (game side)

| File | What | Pure C++ (tested)? |
|---|---|---|
| `NpcLog.h/.cpp` | the player log: events, truncating on rewind, folding regard and flags | yes |
| `Npc.h/.cpp` | `NpcDef` (traits, tastes, ties, life script, schedule), `NpcState Evaluate(def, days, log)` | yes |
| `Schedule.h/.cpp` | activity and place at (day, hour, weather, era); routes on the waypoint graph | yes |
| `Dialog.h/.cpp` | load `.dialog`, evaluate conditions, choose topics, apply effects | yes |
| `Missions.h/.cpp` | load `.mission`, objectives, deadlines, generated notice-board jobs | yes |
| `NpcWorld.h/.cpp` | spawning puppets by LOD, walking, animation, talking (E), the dialog box, the journal | no (needs `Engine`) |
| `Content/Npcs/*.npc`, `Content/Dialog/`, `Content/Missions/` | authored data | (validated by a test) |
| `Config.h`, new "NPCs" section | walk speed, LOD distances, regard gains, absence fade rates, tier thresholds | |
| `Tests/NpcTests.cpp` | determinism, rewind (evaluating after truncation matches never having lived it), absence fade, schedule continuity across midnight, every data file valid | |

**Two things it depends on that don't exist yet:**
- **A save game (decided: built in NPC milestone 2).** The game has `SaveState`/`LoadState` for hot reload, but nothing persists between sessions. The save is game side, JSON under `Saved/`: the outside clock and your own time, money, backpack and tools, the player log, mission states, letters read, and the player-side memories that survive rewinds (déjà vu, the clockmaker). The mined cave also needs saving (the seed plus the changes to the grid, and the loose block piles); that part is bigger and could come in a later step, with a save that holds only the town side first.
- **A JSON reader the game can use.** The engine reads scene files internally, but no JSON API is exported to game modules. Options: a small reader in game code, or exporting the one the engine already uses. The second is an engine change, which needs the user's OK.

---

## 10. Milestone plan (proposed)

Each one builds and runs on its own, like the visual milestones.

1. **People in the valley:** placeholder voxel puppets (written by the placeholder tool), schedules, the waypoint graph, walking with LOD; the first cast (the trading post keeper, the clockmaker and two or three campers) living their day by the clock. Time-lapse figures seen from the mine. No talking yet. This is also "the valley alive" from Design.md §8.
   **Built (2026-11-23, not yet tested by the user):**
   - **The cast** (`Npcs.h`, `Npcs.cpp`): Agnes Rook (trading post), Tom and Mae Harlan (prospector and cook), Bram Kettle (smith), all in Camp only, and the Clockmaker (every era). Each has placeholder traits, ties, a story line and a day with rain alternatives. 24 named places on the props (counter, campfire seats, tent beds, anvil, workbench...) and on the river bank.
   - **Where they are** (`NpcTown.h`): computed from the outside clock alone (tested: same date, same answer; nobody jumps; every walk ends before the next block; no route through water; two people never share a spot; rain sends them to their rain places). Block starts vary up to 20 minutes a day. Routes follow the trails (`cfg::kPathSegments` joined where their ends come close over dry land), straight across under 40 m, and through doorways into buildings.
   - **Puppets** (`NpcWorld.h`): head, torso, arms and legs (`Content/Models/Npcs/<id>/`, written by `UndertimePlaceholders` at 1/16 m blocks), animated in code: walking, standing, working, hammering, chopping, cooking, sitting, panning, sleeping on their backs. They turn their heads to you within 7 m, and block you while they stand still within 12 m.
   - **Time-lapse:** above 1.5x the surface rate they fade (to 40% of their pixels at 4.5x, zone 1 being 5x), two fainter echoes trail them, and they cast no shadows; from 30x they fade out, gone at 80x. Engine: `MeshRenderer::castShadows`.
   - **Debug:** the Game panel's "People" section lists everyone (what, where, how far), "Go to" puts you in front of them, and "Show trails, places and routes" draws the trail graph, every place and the routes being walked.
   - **Not yet:** they aren't shown in the editor; nobody lives in the valley in Settlement and later except the clockmaker (the Settlement cast comes with NPC milestone 4); people walk through each other and through tents on straight paths across the camp; the time-lapse was checked in code and tests only, not by eye.
2. **Talking and regard:** E to talk (the clock pauses), the dialog box with portraits, topics and greetings, barks, regard tiers, absence fading, gossip. The player log and the save game.
   **Built (2026-11-23, not yet tested by the user):**
   - **Talking:** look at someone within 3 m and press E ("E  Talk to the keeper of the trading post" until you've met). Outside time stops, the view turns to their face, standing people stop and face you (sitting ones turn their heads). The box at the bottom shows their portrait (their head model), name, role and tier, the line, and numbered options (keys 1-9 or clicks); E, Space or Enter continue; Esc leaves. Sleeping people can't be talked to.
   - **Dialog files** (`Content/Dialog/<id>.dialog`, JSON, `Dialog.h` documents the format): greetings, farewells, barks, gift reactions and topics, each with conditions (tier, met, days away, hour, era, rain, night, déjà vu, flags, done topics, voice) and effects (friendship, flags, end). `Shared.dialog` has lines anyone can use, by voice. Text fills in `{name}`, `{away}`, `{tie:wife}` and so on. Placeholder lines for the five people. A test loads every file; a typo (unknown key, person, tier, broken JSON) fails it.
   - **Friendship** (`NpcLog.h`): worked out from the player log. The first conversation each outside day, choices in topics and gifts (one a day counts; liked, neutral, disliked) change it; after a week away it fades, half every 60 days (longer for patient people and close friends); gossip passes 30% of a big change to the people they're tied to two days later. Tiers: Hostile, Wary, Stranger, Acquaintance, Friendly, Friend, Close.
   - **Turning time back** (only a debug button so far, "Turn time back a year (test)"): people met after that day forget you; you keep a déjà vu of them (their "dejaVu" greetings); the clockmaker remembers everything.
   - **Barks:** passing within 5 m, someone may say a line (at most every 45 s each, 9 s between anyone), shown as a subtitle.
   - **The save game** (`SaveGame.h`, `<project>/Saved/SaveGame.json`): the clock, where you stand, backpack, gear and the player log. Saved after every conversation, on coming out of the cave, every 2 minutes and when you stop; development views never save. The title screen offers "C  continue". Engine: nlohmann/json is now public, so the game reads and writes JSON with it.
   - **Debug:** the "People" section shows each person's tier and friendship, with -10/+10 buttons, "Forget everyone" and the time-back test. `UNDERTIME_DEV_TALK="id [continues]"` (Screenshot.ps1 `-Talk`) opens a conversation at start.
   - **Not yet:** the mined cave isn't saved (continuing rebuilds the mountain from its seed, unmined, with your backpack); portraits are lit by the scene's light, so they're dark at night; dialog is only placeholder lines.
3. **Missions:** the mission files and journal, outside-time deadlines with the zone translation, the deliver, retrieve, courier, be-there and wait types; the notice board with generated jobs.
   **Built (2026-11-23, not yet tested by the user):**
   - **Mission files** (`Content/Missions/<id>.mission`, format at the top of `Missions.h`): who gives it, when it's offered (a dialog condition), what they say (offer, thanks, news, done, too late), the objective, a deadline in outside days, the reward (friendship, backpack items, a flag) and the friendship lost on failing. A test loads every file.
   - **Five kinds:** deliver (items from the backpack), retrieve (a thing lying in a cavern of a given zone, glowing a little: E picks it up; not in the zone's entry or exit cavern), courier (carry something to another person), meet (be at a place on a day between two hours) and wait (come back after that much outside time).
   - **In conversations:** someone with work offers it first ("Need a hand? (Copper for the roof)"); then you can ask about it, hand things over, return what you found, or give a courier's delivery to the one it's for. Accepting, finishing and failing are log entries, so turning time back undoes them, and rewards can be earned again.
   - **Deadlines:** past one, the mission fails on its deadline day and costs friendship ("Too late: ..."); the giver says their "too late" line once. The journal gives the time left at every depth: "10 days left: 2 hours 29 min outside, 29 min in zone 1, 9 min in zone 2, 1 min in zone 3, under a minute in zone 4".
   - **The journal (Tab):** what's running, for whom, your progress (have 12 of 15 Copper; somewhere in zone 1; take the list to Agnes; be there on 21 November from 18:00), the time left, and the last few done or missed.
   - **The notice board** (a new placeholder model, beside the trading post's porch, Camp only): three notices each outside week, made from `Board.mission` templates (deliver a range of stone or copper, carry a letter to one of several people), each posted by someone living there; the same week always shows the same notices. E reads it; "Take it on" accepts.
   - **Placeholder missions:** Agnes (15 copper for the roof, 10 days), Tom (his lucky pan, lost in zone 1, 14 days), Mae (supper at the fire the evening after next; her list for Agnes), Bram (the old miners' tool box in zone 2, a month), the Clockmaker (come back after a year).
   - **Development:** `UNDERTIME_DEV_MISSIONS="ids... [journal|board]"` (Screenshot.ps1 `-Missions`).
   - **Not yet:** there's no money, so rewards are friendship and backpack items; the board stands only in the Camp era (it moves to the saloon with the Settlement cast); lost things are placed for the current mountain, so a new mountain (F8) moves them.
4. **Lives across the eras:** life scripts, ageing, roles changing hands, the Settlement shopkeepers, descendants, town news when you come up, letters, weddings, funerals and festivals, the graveyard, rewind correctness (tested), déjà vu and the clockmaker's memory.
   **Built (2026-11-23, not yet tested by the user):**
   - **Lives** (`NpcDef` in `Npcs.h`, `Life.h`): born, arriving, leaving, dying (years since you arrived), a spouse and wedding date, parents, roles that change hands, an epitaph, and a day for each era (`eraDays`). A story flag can move a date: Dr Calloway's remedy (`doc_bram`, deliver 5 silver in year 7-8.4) keeps Bram alive past 8.5 years; turning time back before it, he dies on the old day again.
   - **The Settlement's people** (placeholders, era 2, with places inside its buildings): Agnes keeps the general store and lives in the widow's house with Bram, who works the blacksmith's; Tom, Mae and their daughter Ella (born in year 3, drawn smaller until 16) in the white house; Silas Merrow (assayer), Hattie Pell (postmistress), Jonah Quill (saloon keeper), Dr Ruth Calloway (doctor, house calls in the morning). Evenings at the saloon's tables. Placeholder lives: Bram dies at 8.5 years (unless saved), Tom at 9. Later eras have no town yet, so nobody but the clockmaker lives there.
   - **Gatherings:** Agnes and Bram's wedding (year 1.5, at the campfire, 16:00-20:00), funerals three days after a death (at the graves by the chapel, over the footbridge, 10:00-13:00), Founders' Day every year in the Settlement to Industry (day 200, round the clock tower, 17:00-22:00). Everyone living there goes, setting off early enough to arrive; people stand in a ring facing the middle. Being there pleases the hosts (the couple, the dead one's family and friends); missing a close friend's wedding or funeral costs friendship.
   - **The graveyard:** a grave for each of the cast who has died, in a row beside the chapel's two (from the Settlement on); E reads the name, years and epitaph, and "A friend of the courier." if you were close.
   - **News:** coming up from the cave, the summary adds what happened ("Bram Kettle has died.", "Ella Harlan was born to Tom Harlan and Mae Harlan.") and how many letters wait.
   - **Letters** (`Letters.h`, "letters" in each dialog file): written when you've been away too long (per writer) or about a wedding, birth or death you weren't there for, only by people who know you and live there. They wait at the post: ask Agnes in the Camp, Hattie in the Settlement ("Any letters for me? (3)"); the journal says how many wait.
   - **Families:** a child starts with half their parents' best friendship at the birth ("Pa told me all about you").
   - **Also:** the notice board moves to the saloon in the Settlement; old people stoop; walkers called away mid-walk set off from where they are (no jumps); the footbridge is walkable; the People panel shows ages and why someone isn't there (not born, died, not here yet, no town in this era).
   - **Not yet:** later eras' towns (Boomtown on) and their people; the graveyard only shows the cast's graves (no names on the two old ones); funerals and weddings have no special poses or decorations.
5. **Content and tools:** writing the residents' stories (Design.md §5), hand-made models replacing the placeholders, a dialog view in the editor, an NPC inspector ("preview this NPC at year X").

---

## 11. Other suggestions

The clockmaker's memory, déjà vu, time-lapse figures, letters and dated events were taken into the design (2026-11-23). Still only ideas:

- **Gifts that age:** a cask you give at a wedding is opened at the child's wedding 25 years later.
- **A camera or sketchbook (an idea):** keep pictures of people; once they're gone, that's what's left.
- **Don't build Sims-style needs.** They're random, expensive, and fight both the time rule and rewinding. Schedules plus life scripts plus a daily mood give the same feeling of life and stay deterministic.

---

## 12. Questions for the user

All answered (2026-11-23); see "Decided" at the top. Still to settle later, when the work gets there:
- the voxel size of the hand-made people (with the first real model);
- the residents' stories (Design.md §5);
- the numbers: regard gains, fade rates, tier thresholds, time-lapse rates (`Config.h`, tuned in play).
