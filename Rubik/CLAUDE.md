# Rubik (CLAUDE.md for Rubik/)

Guidance for working on the Rubik's cube game. The engine, the build and the branch rules are in the root `CLAUDE.md`.

- **What it is:** the first game on CherryEngine and the default module (`CHERRY_ACTIVE_GAME_TARGET` is `RubikGame`, so the monolithic build runs it). Its controls are in `README.md`.
- **Code:** `RubikPuzzle` is the pure puzzle logic (tested by `RubikPuzzleTests`); `Rubik`, `RubikManager` and `RubikModule` put it in the engine. `Scripts/` holds its self-registering scripts (`Rotate`, `Test`).
- **Branches:** Rubik work happens on `rubik/<topic>` branches (see "Branches and commits" in the root `CLAUDE.md`).
