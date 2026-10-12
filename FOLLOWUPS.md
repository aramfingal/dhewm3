# Known gaps and follow-ups

Each item: where, approach, risk, how to verify.

## Harness

1. **Frozen-world views only.** `views_*` scenarios stop game time (`g_stopTime 1`) before
   moving the camera, so particles, animated lights and moving models are compared only in
   the `intro_*` scenario and the timedemo (no image compare there). Approach: record a
   `recordDemo` per map and take shots at fixed demo frames. Verify: two GL runs must match.
2. **The engine log (`dhewm3log.txt`) is written to `~/Library/Application Support/dhewm3`**
   even with a throwaway `fs_savepath`: the path is fixed before cvars are read
   (`neo/sys/osx/DOOMController.mm`). Harmless, but not isolated.
3. **Intro shots are not exactly repeatable** (up to about 0.1% of pixels between two GL
   runs): the cinematic is running while the shot is taken.
