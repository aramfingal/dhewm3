# Frame rate independent of the game tic (`com_smoothFrames`)

Doom 3 runs its game code (physics, scripts, AI, animation) in fixed steps of 1/60 s, and
stock dhewm3 draws exactly one frame per step, so the game is limited to 60 frames per
second. This fork separates the two: the game still runs 60 tics per second, exactly as
designed, and the renderer draws as many frames as it can, each showing the world
interpolated between the last two tics. Physics and scripts are untouched; the frame rate is
whatever the machine or `com_maxFPS` allows.

The upstream project has an unmerged branch (`kickstart-my-hertz`) that takes the other
route, a configurable tic rate (`com_gameHz`, up to 250). That changes how the simulation
itself behaves and needed fixes all over the physics code; it also still ties frames to
tics. The two approaches can be combined later.

## Cvars

| cvar | default | |
|---|---|---|
| `com_smoothFrames` | 1 | 0 = one frame per game tic, as stock dhewm3 (also in the F10 settings menu, Video Options) |
| `com_maxFPS` | 0 | frame rate limit with `com_smoothFrames`, 0 = none (vsync, `r_swapInterval 1`, still applies) |
| `com_smoothFramesDirectLook` | 1 | turn the view with the mouse in every frame (see below) |
| `r_interpolateMaxDistance` | 128 | a move farther than this in one tic is a teleport and is not interpolated |
| `com_printFPS N` | 0 | print the average frame rate every N frames |
| `com_smoothFramesDebug`, `r_interpolateLog` | | for the test harness |

Frames are tied to tics again, whatever `com_smoothFrames` says, in multiplayer, while a
demo plays or records (`timeDemo`, `recordDemo`, `aviDemo`), and with `com_fixedTic` or
`com_minTics` set.

## How it works

```
real time   ------|----------------|----------------|------->
game tics         n-1              n                n+1         (60 per second)
frames              f  f  f  f  f    f  f  f  f  f    f  f       (as many as possible)
a frame shows       the world between tic n-1 and tic n, alpha = part of a tic since tic n
```

What is on screen is up to one tic (16.7 ms) older than the game state. That delay is the
price of interpolating instead of extrapolating: nothing ever has to be corrected.

- **The loop** (`neo/framework/Session.cpp`, `Common.cpp`): `idSessionLocal::Frame` no longer
  waits for the next tic. It runs the game tics that have become due (none in most frames)
  and the frame is drawn at once. `Com_TicFraction` gives alpha from the tic clock.
- **Entities and lights** (`neo/renderer/RenderWorld_interp.cpp`): the game tells the render
  world where things are once per tic (`UpdateEntityDef`, `UpdateLightDef`). The render
  world remembers, for each one, where the game last put it and where it was before that
  tic. Before a frame is drawn, everything that moved in the latest tic is placed in between
  (position linear, orientation spherical). Things that did not move are not touched.
- **The view**: the game computes the view once per tic. The render world keeps the views
  of the latest tic and the one before (per `RenderScene` call, so portal skies work) and
  draws with position, direction, field of view and time in between. The interpolated time
  makes particles and animated materials move smoothly too.
- **Skeletons** (`R_InterpJoints`): the game poses a skeleton once per tic. The renderer
  keeps the poses of the last two tics for entities in view and skins the pose in between.
- **Looking around** (direct look): a view that follows the mouse one tic late feels
  sluggish. While the player controls the view (the game code says so every tic by setting
  `com_viewFollowsInput`; not in cinematics, when dead, in third person or on a mounted gun),
  a frame's view direction is that of the latest tic plus the input since then, and whatever
  is attached to the view (the first person weapon, muzzle flash, flashlight: entities and
  lights that are only drawn in that view) turns with it.
- **Console scripts**: `wait N` used to count frames (in fact calls of
  `ExecuteCommandBuffer`, which also happen per input event). With `com_smoothFrames` it
  counts game tics, so a wait is as long as it was at 60 fps; otherwise it counts frames.

Other things that depended on one frame per tic and were changed: keyboard turning speed
(`idUsercmdGenLocal::AdjustAngles`, now by elapsed time), the drift of blood splats on the
screen (`idPlayerView`, once per game frame), and `com_frameTime`, which now advances with
frames so menus and the console animate at the frame rate.

## What still runs at 60 Hz

By design: everything the game code does, including the HUD and in-world GUI screens (they
are drawn every frame, but their content changes per tic) and sound. Weapon kick, bob and
shake are computed per tic and interpolated like everything else.

Not interpolated: shader parameters (light colours, beam end points), bounds, and anything a
model computes from the game time rather than the view time.

## Testing

`misc/metal-test/metal_harness.py` has three kinds of checks (see its README):

- `smooth_base_alphalabs1`: fixed states through `com_smoothFramesDebug`, compared within
  one session: a frame half way between two tics equals a frame taken from half way, alpha 0
  equals the tic before, and a view turned 10 degrees by "input since the last tic" equals
  really facing there, weapon included.
- `uncapped_*`: maps played with frames uncapped; fails if the frame rate is not above the
  tic rate.
- `smooth_intro_base_mars_city1`: during the opening cinematic, a frame at alpha 0 must
  reproduce the frame of the tic before.
- Camera path analysis with `r_interpolateLog 1` during the mars_city1 intro: the camera
  moves every frame in proportion to the time passed, with no backward steps.
