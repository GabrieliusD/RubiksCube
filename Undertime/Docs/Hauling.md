# UNDERTIME: mining, hauling, crafting, machines and the claim

Status (2026-11-23): **all milestones H1-H12 built** on branch `features/hauling` (from the finished NPC work). **Not yet tested by the user.** The game version is 0.7. The plan follows "Built" below, as it was agreed.

## Built (2026-11-23, not yet tested by the user)

How to try it all without playing for hours: `UNDERTIME_DEV_HAUL` (or `Screenshot.ps1 -Haul`) sets up a scene. Its words are listed at the bottom of `Hauling.cpp`:
- `cents N`, `give id N`: money and goods.
- `barrow N`, `hold`, `tip`: a barrow next to you, held or tipping.
- `yard N`: stone in the yard.
- `shop N`: open a shop.
- `claim`, `house`: stand at your claim; build a sample house and field.
- `menu pieces|bench|store|cook`: open a window.
- `place item x y z dx dz`: place a thing where a ray from that point hits.
- `fast N`: outside time 10^N faster (machines speed up too).
- `tool N`: pick the tool.

Development runs never save.

- **H1 money and selling** (`Economy.h/.cpp`, `Hauling.cpp`):
  - Money is shown in dollars and counted in cents. You start with $5.
  - **Shops:** talking to a shopkeeper offers a "Let's trade." topic, which opens their shop window (outside time stops, as in a conversation).
  - **Who keeps a shop** follows the person's role at the time:
    - Agnes: the trading post, then the general store;
    - Bram: the smithy;
    - Mae, later Jonah: the kitchen;
    - Silas: the assay office.
  - **Prices:** ore sells for its base price × the era's factor; the assayer pays 10% more.
  - **The yard:** stone (and anything else) that settles in the fenced yard north of the trading post is paid once (5 cents a stone block in Camp) and becomes the town's. The shovel leaves the town's blocks alone. The heap is saved and comes back as a mound when you load.
  - **Upgrades:** picks, packs and lanterns set your gear. A copper pick breaks the first sealed band, silver the second, gold-edged the rest: the sealing rock in the bite turns to stone and is carved.
- **H2 the wheelbarrow** (`Barrow.cpp`):
  - **Buying:** bought at the trading post or store; it waits by the yard gate.
  - **Pushing:** E pushes it, and its load slows you, down to 45% when full. While you push, the pick and shovel are put away.
  - **Filling:** parked within reach, the shovel fills it (B: the pack instead). R puts your pack's stone in it.
  - **Tipping:** Q while pushing tips the load out in front, as live blocks.
  - **Saved:** where it stands and its load. A barrow that falls out of the world comes back at the mine mouth.
- **H3 orders:** the shop window's Buy list, with what each thing needs (money and ore). The axe, picks, packs, lanterns, seeds, food, timber, oil, leather, coal, glass, machines and machine parts, each by era and shop.
- **H4 your claim and building** (`Building.h/.cpp` are pure and tested; `Claim.cpp`):
  - **The plot:** a 24 m square at (100, -40), flattened with a ground pad and cleared of trees. A trail runs to it from the mine trail.
  - **What's there at the start:** a tent just south of it (E sleeps, from 20:00 to 6:00) and a starter workbench.
  - **The hammer (key 3):**
    - right mouse opens the piece list (your things first, then the claim's pieces, grouped);
    - a dithered ghost and an outline show where the piece goes; R turns cell pieces;
    - left mouse sets a site down and builds it up swing by swing, taking materials from your pack, a barrow parked within 4 m, then your crates;
    - X held takes a piece down, and everything paid comes back (stone as loose blocks).
  - **Support:**
    - walls stand on the ground, or on a wall below or a floor beside them;
    - floors rest on a wall below one of their edges, and hang out up to 2 cells;
    - roofs chain up to 3 cells from a wall;
    - furniture holds nothing up;
    - a piece something rests on can't be taken down.
  - **The pieces:**
    - stone foundation, plank floor, farm plot;
    - stone and plank walls, a wall with a window hole, a glass window (Settlement), a half wall;
    - a doorway and a door (2 m; E opens it);
    - roof slope, roof ridge, stairs;
    - fence, gate;
    - workbench, crate, fire pit, bed, table, stove (Settlement), lamp (a point light).
  - **Drawing:** all pieces are one merged mesh and one static collision mesh, rebuilt when a piece changes.
  - **Crates:** one shared store, with a put-in and take-out window.
- **H4b your workbench and placed things** (`Placeables.cpp`):
  - **The bench** makes timber from logs (1 log gives 4), timber props, ladders, wooden chutes and torches.
  - **Your things** go anywhere with the hammer:
    - **props** stand on a floor under a rock roof, and the voxel they press on counts as held up (`DetachUnsupported`'s `held`), so rock touching it never caves in;
    - **ladders** go against a wall: walk into one facing it and hold W to climb, and you step off at the top;
    - **chutes** are a smooth 45° ramp with sides that blocks slide down;
    - **torches** burn for 2 hours of their own time, then go out.
  - X held picks a thing up again.
  - **Engine:** none (the player controller gained `climbing`).
- **H5 hunger** (`Food.cpp`):
  - The meter drains over 20 minutes of your own time, the same at any depth.
  - F eats the most filling thing you carry. A fire pit or stove (E) cooks potatoes, pumpkins, wheat and apples into meals that fill more.
  - **Empty:** you're weaker: the pick at 55%, more axe chops, a slower barrow.
  - **A night's sleep** empties a quarter of the meter.
  - Food is sold at the trading post and the kitchen.
- **H6 wood** (`Wood.cpp`; `ValleyScenery::SetTreeGrowth`):
  - **Felling:** the axe (key 4, from the smith) fells a valley tree in 5 chops (more when hungry), giving 1 to 4 logs by size. The big tree can't be felled.
  - **Growing back:** a felled tree leaves a stump. From the day it was felled it becomes a sapling after 40 outside days and a full tree after 3 years. Its trunk blocks you from half grown: that collision chunk is rebuilt without it, then with it again.
  - **Rewinds:** turning time back past the felling brings the tree back; the logs stay yours.
- **H7 farming** (`Farm.cpp`, `Growth.h`):
  - **Planting:** build farm plots with the hammer (free). E on an empty plot plants the first seed you carry.
  - **The crops:**

    | Crop | Ripe after | Stays ripe |
    |---|---|---|
    | Potatoes | 3 days | 4 days |
    | Wheat | 10 days | 6 days |
    | Pumpkins | 30 days | 15 days |
    | Apple sapling | 3 years | every autumn after |

  - **Harvest and spoiling:** E harvests a ripe crop into your pack. A spoiled one is cleared.
  - **Looks:** each plot shows its stage: growing, ripe, gone to seed.
- **H8 machine time and drill rigs** (`Machines.cpp`):
  - **Own time:** every placed thing works at `OwnRate(where it stands)` = your outside rate ÷ the rate there. A zone 1 drill runs about 21 times your pace while you're in zone 3, and the surface fastest.
  - **What a drill does:** it is set against a rock face in the cave and bores a 1 m × 1.5 m tunnel straight in. Its blocks go into a 600-block hopper, ore kept apart. Each frame it carves through `ApplyWorldChange`, so cave-ins happen.
  - **What stops it:** a full hopper, running out of coal, sealing rock (only the electric drill breaks bands), or wear (120 own hours: E with machine parts mends it).
  - **The three drills:**
    - the hand borer works only while you're beside it;
    - the steam drill burns 1 coal an own hour (E adds coal);
    - the electric drill needs the power line (Industry).
  - **E:** empties the hopper into your pack and starts a stopped drill again.
- **H9 belts and the steam engine:**
  - **Placing:** 1 m belts are placed in your facing direction.
  - **How blocks move:** the first belt of a line takes from a drill or the mill's ore output just behind it. Each belt passes blocks to the next at 2 blocks an own second, holding 30 each. The last drops them as live blocks (6 a frame at most).
  - **Power:** belts run while a steam engine with coal is within 30 m, or always from Industry on (the power line).
  - **Loose blocks** on a belt ride along it.
- **H10 the stamp mill:**
  - **Where:** within 16 m of the water wheel.
  - **Using it:** E tips in the stone from your pack and a barrow beside it, up to 2,000 in the bin, and takes out the ore.
  - **Output:** it crushes 600 stone an own hour and washes out 4% copper, plus 1.5% silver from the Settlement on. Its tailings heap grows beside it. It wears out (parts).
- **H11 rails and the hand cart:**
  - **Rails:** laid in 1 m pieces, lined up with the rail they continue.
  - **The cart:** set on a rail. E pushes it along the line (it stays about 1.3 m ahead of you), and released it rolls downhill and slows by friction. The shovel fills it (400). Q tips it out to the side.
- **H12 town projects** (`econ::Projects`):
  - **How they're built:** each era has projects:
    - Camp: the camp's stone hearth, the trading post's floor;
    - Settlement: the clock tower's base, the bridge piers;
    - Boomtown: the bank's vault, the engine house;
    - Industry: the chapel's bell tower.
  - **Using up the yard:** the town takes 120 of the yard's stone an outside day for the current era's first unfinished project; the heap visibly goes down (highest blocks first).
  - **Finishing one** pays you the town's thanks ($15 to $200) and tells you what changed.
  - **Where to see them:** the notice board lists the projects and their progress.
- **Tests** (`Tests/HaulingTests.cpp`):
  - prices and shops;
  - building: costs spread over swings, taking down gives everything back, support rules, sites;
  - trees and crops by day (a 9 minute zone 1 dive brings you up to ripe potatoes);
  - the project table;
  - the machine time rule.
- **Not done / known limits:**
  - **The mined cave still isn't saved:** continuing rebuilds the mountain unmined, so props and drills in the cave stand in rock again after a load.
  - **No seller for machines in later eras:** the electric drill and electric lamp are sold only at the general store, whose keeper (Agnes) dies 24 years in, before the Industry era, so nobody sells them yet. The smith (Bram) dies at 8.5 years unless saved. The later eras' cast will fix it (NPC milestone 4).
  - **Logs** go straight into the pack (not loose logs to pick up).
  - **The barrow** moves through rock if you push it into a wall.
  - **The cart's load** isn't drawn.
  - **Claim rooms:** the claim's rooms don't get the indoor light of the town's houses yet.
  - **Rewinds:** town projects aren't undone by turning time back.
  - **Not decided:** the governor.

## Decided (2026-11-23, the user's answers)

- **Carrying:**
  - The backpack holds stone and ore, as now (`kBackpackCapacity`). A load in the backpack doesn't slow you.
  - A **wheelbarrow**, filled with the shovel, carries far more. Its load does slow you.
- **Selling stone:** you tip it into the trading post's **yard**. You're paid per block, and the blocks stay there as a growing heap that town projects later use up. No block is ever lost.
- **Crafting is done by townspeople:** the smith, the trading post and the store.
  - Orders are **instant**: coins plus materials.
  - What you can order depends on who lives in the valley in that era.
  - **You craft basic things yourself** at a workbench on your claim.
- **Mining automation:** drill rigs, chutes and conveyors, and an ore mill. Hired help and mule teams aren't in this plan.
- **Machines work in their own time,** at the rate of where they stand (see "Machines" below).
- **Rewinds never touch the cave.** Machines, props and chutes in the cave keep their state, and their fuel stays spent.
- **The claim** is away from town (see "The claim" below).
- **You build your claim yourself:** **pieces snapped to a grid** (walls, floors, roofs, doors and so on), each **built by hand** with a hammer, from your stone and timber (see "Building the claim").
- **Machines stay through rewinds,** on the surface too (the mill included).
- **Hunger:** a meter that drains in **your own time**. Running out makes you **weaker** (see "Hunger and food").
- **Wood:** you can fell trees, and they **grow back** with outside time.
- **Farming** on the claim, **timed by outside days**: crops ripen, stay ripe for a while, then spoil.
- **Undecided:** the clockmaker's governor (see "Still open").

## The loop

1. **Mine:** the pick turns rock into loose blocks (built).
2. **Load:** the shovel scoops blocks into the backpack, or into the barrow when it's parked within reach.
3. **Haul:** push the barrow out of the mine, or let chutes and conveyors carry the blocks.
4. **Sell:** tip stone in the trading post yard and sell ore at the counter. Prices change by era.
5. **Order and craft:** tools, carriers and machines from the townspeople; props, chutes and ladders at your bench, from timber you buy or fell.
6. **Plant and eat:** sow before a dive, sized so you come up at harvest. Eat before you go down.
7. **Automate:** a shallow drill works a seam while you dive deep.
8. **Dig deeper:** better picks break the sealed bands. Deeper ore is worth more, and time outside runs faster.

Farming and machines both reward knowing how long a dive will take outside. A wrong guess costs you: the crops spoil or the drill runs dry.

## The claim

- **Where:** a meadow on the foothills east of the mine, at about (100, -40) in valley coordinates (`cfg::kClaimCenter`, about 25 m across).
  - **Distances:** about 75 m from the mine mouth, about 150 m from the town square, about 55 m from the river.
  - **On the map:** it's out of sight of the town, with the big tree between them, and has a view down the valley.
  - **Getting there:** a new trail branches off the mine trail, plus a clearing (`kClearings`).
  - The spot is to be checked in a screenshot, and is easy to move.
- **What you start with:** an empty meadow, a tent (sleeping skips the night) and a starter workbench. Everything else, you build.
- **Shielded from rewinds,** along with everything on it: buildings, the crate, crops and machines. The machine that shields it is to be designed.

## Building the claim (pieces on a grid, by hand)

- **The hammer:** a third tool, on key 3.
  - **Picking a piece:** hold right mouse for a wheel of pieces, grouped as Foundations, Walls, Floors, Roofs, Doors and windows, Fences, and Furniture and work.
  - **Placing:** a ghost of the piece snaps to the grid where you aim. R turns it.
  - **Building it:** left mouse sets the ghost down as a site, then each swing builds it up.
    - Each swing takes some of the materials from your backpack, or from the crate when you're within reach of it.
    - It takes a few swings for timber and more for stone.
    - The piece fills in from the bottom as you swing.
  - **Taking it down:** hold left mouse on a finished piece. It gives back **all** its materials, and stone comes back as loose blocks, so nothing is ever lost.
- **The grid:** 1 m cells horizontally and 1 m steps vertically. Walls sit on cell edges and are 0.25 m thick: one block, the game's voxel size.
  - Every piece is a small `.vox` model (placeholders from `UndertimePlaceholders`), so the claim matches the rest of the style.
- **What pieces cost:** stone pieces cost exactly the blocks they're made of. A 1 × 1 m stone wall, 0.25 m thick, is 4 × 4 × 1 = **16 stone blocks**. That makes the stone you mine and haul the building material, directly.
- **The pieces:**

| Group | Pieces | Material | From era |
|---|---|---|---|
| Foundations | stone foundation (1 × 1 m slab, levels the ground), stone step | stone | Camp |
| Walls | wall, half wall, wall with window hole, doorway, corner pillar | stone or timber | Camp |
| Floors | plank floor, stone floor, hatch | timber / stone | Camp |
| Roofs | 45° roof, roof edge, ridge, flat roof | timber; shingles later | Camp |
| Doors and windows | door (opens), shutters, glass window | timber; glass from the store | door Camp, glass Settlement |
| Stairs | stairs, ladder | timber | Camp |
| Fences | fence, gate, farm plot fence | timber | Camp |
| Furniture and work | workbench, crate, fire pit, bed, table, chimney stove, lamp hook, sign | timber, stone, copper | Camp; the stove Settlement |
| Later | brick walls (stone + coal, at the smith's kiln), painted planks, a well, the rewind-shield machine | | Boomtown, Industry |

- **Support (kept simple):** a piece has to touch the ground or a piece that does (foundations on ground, walls on foundations or floors, roofs on walls).
  - A piece holding others up can't be taken down until they're gone.
  - There's no collapse physics.
- **Bounds:** only inside the claim's boundary (`kClaimRadius`), shown with corner stakes. A bigger claim could be bought from the assay office later (idea).
- **Indoors:** an enclosed space with walls, a roof and a door gets a room (`RenderSettings::rooms`, as the town's houses do), so inside is dim and lamps matter.
  - Sleeping in your own bed instead of the tent restores the hunger meter a little more (idea).
- **Rendering and collision:** all finished pieces are merged into one mesh (rebuilt when a piece changes, like a loose-block pile) and one static collision body from their boxes.
  - Sites under construction are drawn separately, with the filled-in part solid and the rest as a dithered ghost (`MeshRenderer::dither`).
  - **Engine:** none expected (`BakeVoxelMesh`, `BakeVoxelBoxes`, `CreateStaticMesh` exist).
- **The save game:** a list of pieces (type, cell, turn, and how built it is if it's a site).

## Hunger and food

- **The meter:** it drains in your **own time**, which is real time. A 10-minute dive costs the same hunger in zone 1 as in zone 5.
  - So hunger limits how long you stay down in your own time, as lantern oil does, not how much outside time passes.
  - A full meter lasts `kHungerMinutes` of your own time (say 20).
- **When it's empty (decided):** you get **weaker**, with no death. Pick and hammer swings do less, and the barrow is harder to push, until you eat. How much weaker goes in `Config.h`.
- **Eating:** E on food in the backpack, or at the fire pit, the campfire or the saloon.
  - Better food fills more, and cooked food more than raw.
  - Food you carry doesn't spoil in your own time.
- **Where food comes from:**
  - your farm (raw, or cooked at the fire pit);
  - the trading post and store, at era prices;
  - meals at the saloon (Settlement and later);
  - fish and foraging, later ideas.
- Food also works as a gift to NPCs; the gift system is in `features/npcs`.
- **Lunch pail (idea):** a carried meal that tops you up deep down.

## Wood

- **The axe:** sold by the smith. Swing it at a valley tree's trunk.
  - A few hits fell it, the tree topples, and it leaves logs (loose items, like blocks) and a stump.
  - The shovel or E picks the logs up. The bench turns logs into timber.
- **Regrowth:** worked out from the day it was felled, so no per-frame simulation.
  - A stump becomes a sapling after some outside time, and a full tree after years (`kTreeRegrowDays`).
  - So a long dive regrows the forest.
- **Rewinds (proposed):** a felled tree outside the claim is a record keyed by the day it was felled.
  - Winding back past that day brings the tree back; the logs you already took stay yours.
  - That's consistent with "only the outside clock goes back." It costs Time crystal, so it's no free farm.
- **Selling:** logs sell to the sawmill (Settlement to Industry). Timber can still be bought.
- **Trees that can't be felled:** the big tree (a story place) and trees on building plots.
- **Technical:** felling removes that tree's entity, its LOD copies and its trunk collision from `ValleyScenery`, and puts a stump in. The felled-tree list goes in the save game.
  - How trees are batched now (an entity per tree or merged into region meshes) decides the work; to check at the start of H6.

## Farming (on the claim)

- **Planting:** buy seeds at the trading post or store, dig a plot with the shovel, and plant with E.
- **Growing:** crops grow in **outside days**, worked out from the day they were planted. Each crop has days to ripen, days it stays ripe, then it goes to seed (spoiled, or worth less).

| Crop | Ripe after | Stays ripe | One dive that fits |
|---|---|---|---|
| Potatoes | 3 outside days | 4 days | about 9 min in zone 1, or 3 min in zone 2 |
| Wheat | 10 days | 6 days | about 10 min in zone 2 |
| Pumpkins | 30 days | 15 days | about 4 min in zone 3 |
| Apple sapling | 3 years to fruit | every autumn after | zone 4 or 5 dives |

  - These are starting numbers in `Config.h`. They follow the zone rates: 8 hours, a day, a week, a month and a year per minute.
- **Seasons** (when they exist) decide what grows. Winter stops most crops, so a dive through winter is safe.
- **The harvest:** E gathers the crop into the backpack. It sells, cooks into meals, or makes gifts.
- **Shielded from rewinds:** the farm is on the claim, so winding time back doesn't un-grow it.
- **Animals:** chickens and a goat, which pay off on short dives. Later.

## Machines work in their own time

Time runs faster with depth for everything, not only the player. A machine counts its work in the time of where it stands.

- **The formula:** your own time is real time. Over one real second, outside time advances by `rate(you)`, and a machine at depth `d` advances `rate(you) / rate(d)` of its own time.
  - A machine shallower than you runs faster than you; one deeper runs slower.
  - Surface machines run at outside time, the fastest of all.
- **Example:** a drill in zone 1 while you're in zone 3 works about 21 times as fast as you (a week per minute ÷ 8 hours per minute).
- **What it means for play:** put machines shallow and dive deep yourself. You come back up to a full hopper.
- **Limits** keep it bounded:
  - **fuel** burns in the machine's own time;
  - **hoppers fill up**, and a full one stops the machine;
  - **wear:** a machine breaks down after enough of its own time and needs the smith.
- **Off-screen work is computed, not simulated,** like the NPCs. Each machine keeps the own time it has used and its stock: fuel, hopper count and wear.
  - It catches up in one step when you come near, or when something reads it.
  - Blocks only become physical near you.
- **Never lost:** drilled blocks are counted in the hopper until they come out.

## Who makes what (orders, instant)

| Crafter | Era | Sells or makes |
|---|---|---|
| Trading post (Agnes) | Camp → | backpack upgrade, lantern, oil, timber, seeds, food, the first wheelbarrow; buys ore and stone |
| Smith (Bram) | Camp → | picks (copper, silver, gold-edged), the axe, rails, cart wheels, drill bits; repairs machines |
| General store | Settlement → | big pack, better lanterns, coal (fuel), more seeds, the hand cart |
| Saloon | Settlement → | meals |
| Sawmill | Settlement → Industry | buys logs, sells timber cheaper |
| Engine works (to be cast) | Boomtown → | steam drill, steam belt, the engine house |
| Power house (to be cast) | Industry → | electric drill, electric belts |
| Clockmaker | every era | the claim's rewind shield, later |

- **Prices:** coins plus ore of the tool's tier. A silver pick needs silver.
- **When a crafter is gone:** their items can't be ordered. You rewind to their era, or make do. Abandoned has leftover stock at high prices.
- **Roles not cast yet:** the engine works and the power house come with NPC milestone 4.

## Your own workbench (basic things)

| Item | Needs | Does |
|---|---|---|
| Timber | logs | from felled trees |
| Timber prop | timber | placed in the cave, holds the roof (an anchor for `Structure.cpp`'s support check) |
| Ladder | timber | placed against a wall, climbable |
| Wooden chute | timber | a sloped trough in 1 m pieces; blocks slide down it (the first automation) |
| Torch | timber + oil | a placed light that burns down in its own time |

Crates, fences and the fire pit are building pieces (see "Building the claim"). The fire pit cooks: raw crops become meals that fill more.

## Automation

### Chutes (Camp, made by you)
- Static sloped collision plus a mesh. Blocks slide down by physics, with low friction.
- They only work near you; off-screen they're only physics, so nothing moves.

### Drill rigs (Settlement hand borer, Boomtown steam, Industry electric)
- **Setting one up:** placed against a rock face and aimed along a direction.
- **Drilling:** it bores a tunnel at a speed set by its tier and the rock's hardness.
  - Sealed bands stop it, unless it has a better bit.
  - **Fuel:** the hand borer runs on your cranks, so only while you're near; steam runs on coal; electric on the power line.
- **The hopper:** it keeps counts by block type, so ore stays apart. Empty it with the shovel, or feed a conveyor. A full hopper stops the drill.
- **Catching up:** it carves along its line through `ApplyWorldChange`, so the support check runs, and cave-ins can bury it.

### Conveyors (Boomtown steam belt, Industry electric)
- 1 m belt pieces joined into a line from a hopper or pile to a crate, cart, rail end or the yard.
- **Near you:** blocks on a belt get a velocity along it.
- **Off-screen:** the line moves counts from its input to its output at its rate, in its own time, until the output is full.

### The ore mill (Settlement →, on the river's water wheel)
- A stamp mill by the existing `Water Wheel` prop. It crushes stone you tip into its bin, at outside time.
- It washes out a little copper, and later silver.
- The waste goes onto a tailings heap, which is never lost.
- It wears in outside time, so a decade away leaves it broken.

## Milestones (proposed)

Each one builds and runs on its own.

1. **H1. Money and selling:**
   - coins (HUD, save game);
   - the trading post counter, with era prices;
   - the yard, where each block is paid for once and then belongs to the town;
   - tests.
2. **H2. The wheelbarrow:** push it, fill it with the shovel, tip it. Its load slows you, and it's saved.
3. **H3. Orders from townspeople:** order lists from talking to them (dialog from `features/npcs`). Picks by tier, the axe, the big pack, the lantern, seeds and food.
4. **H4. The claim and building:**
   - the plot, trail and clearing, the tent (sleeping) and the starter bench;
   - the hammer, the piece wheel, ghost placing, sites built up by swings, and taking pieces down;
   - the grid, support and bounds;
   - the merged mesh and collision, saved pieces;
   - the first pieces: foundations, walls, floors, roofs, a door, stairs, fences, a crate, a fire pit, a bed;
   - tests: grid and support rules, material costs, and that taking a piece down gives everything back.
5. **H4b. The workbench:** props as roof anchors, ladders, chutes, torches.
6. **H5. Hunger and food:**
   - the meter in your own time, and the empty penalties;
   - eating and cooking;
   - food sold in town, and meals.
7. **H6. Wood:**
   - the axe and felling;
   - logs, then timber at the bench;
   - stumps regrowing by the felled day;
   - rewind behaviour;
   - the felled-tree save, and tests (regrowth is pure, from the clock).
8. **H7. Farming:**
   - plots, seeds and crops by outside day (ripe window, going to seed);
   - harvest and cooking;
   - tests (crop state is pure, from the planting day and the clock).
9. **H8. Machine time and drill rigs:**
   - the own-time rule, and the machine record that catches up in one step;
   - the hand borer, then steam;
   - tests: one-step catch-up matches frame by frame; a full hopper stops the drill; fuel and wear limits.
10. **H9. Conveyors and the engine house.**
11. **H10. The ore mill.**
12. **H11. Rails and the hand cart** (Settlement).
13. **H12. The spoil heap and town projects:** notice board missions that draw stone from the yard.

## Still open
- **Numbers:** prices, drill speeds, fuel, wear, the mill's ore fraction, hunger minutes, what each food fills, crop days, regrowth years.
- **The governor** (undecided): should the clockmaker sell a part that holds a machine to your own time?
- **Machine output into the yard:** is it paid like hand-tipped stone? Probably yes.
- **Building:**
  - the exact piece list and costs;
  - whether pieces can be placed outside the claim, such as a shelter at the mine mouth or stone pieces packing mined-out rooms in the cave;
  - whether the claim can grow;
  - which piece wheel layout feels best (to try in play).
- **How much weaker an empty hunger meter makes you.**
