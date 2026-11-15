# 3D stage

The 3D stage places each of the 16 MIDI channels in a room and lets you stand somewhere in it. It is a
post effect: the synth renders its channel buses as before, and the stage turns them into a stereo
mix with direct sound, early reflections and a reverb tail that come from the room you drew. It is off
by default; with it off, or while its room is still being built, the audio is exactly what it was.

## Turning it on

Open the **3D Stage** page in the Configurator and switch the stage on. Changes apply live while the
driver runs: moving a channel or yourself only rebuilds the paths, while changing the room, a surface,
furniture, the ray count or the room amount traces a new tail on a background thread (a few seconds on
the CPU, a fraction of a second with the Vulkan ray-query tracer) and swaps it in when ready. If you
change something while a trace runs, it restarts from the newest settings.

The same settings live in the `stage` section of `config.json`. A device rate or block size change
suspends the stage until the engine restarts.

## What it models

- **Direct sound**: distance delay and level, with a Doppler shift when a channel or you move.
- **Early reflections**: the wall bounces up to third order (image sources), the first ten as live
  taps that follow movement and the rest as a static response.
- **Tail**: energy rays traced through the room, furniture included, per frequency band, scaled so the
  direct to reverberant balance follows the critical distance.
- **Surfaces**: each wall, the floor and the ceiling take a material (plaster, wood, carpet, glass,
  concrete, curtain, foam and so on), which sets absorption per band.
- **Furniture**: boxes that block, absorb and scatter rays and add their own bounces.

## The measured head

With the ear model on, the direct sound goes through one generic human head built from the 96 measured
subjects of the HUTUBS database (CC BY 4.0, TU Berlin; details and how it was made in
`src/render/data/README.md`). It gives the level, spectral and time differences between the ears for any
direction, including elevation and front/back. Reflections keep the plain ear model. The table is
embedded in the driver and the Configurator; setting `SVMS_NO_HRTF=1` switches back to the plain ear
model for comparing.

## Per-channel controls

Each channel has a position, a **level** trim (-24 to +12 dB, scaling its direct sound, early taps and
tail send), an **amount of room** send (35 is neutral, it only scales the tail feed) and a **width**
(drawn as a cloud on the plan, it has no audio effect yet). Dragging **You** moves only you; hold Ctrl to
carry the whole band along.

## Importing a room model

The Room card can load an OBJ file. The room box fits to it, the plan draws its triangles, and groups
or material names (carpet, foam, wood, glass, concrete, curtain) pick a surface for each group, which
you can change. Turn off *Y is up in the file* for files where Z is up.

A model changes the tail and the direct sound only. Early reflections need flat walls, so they are not
modelled for a model, the plan draws only the direct line, and only the first 12000 triangles are used.
A file that cannot be read falls back to the box room.

## Listening tests

The strip at the top of the page plays test material through the room as the page has it now, saved or
not, and stays in view while the page scrolls. It keeps following the page while it plays: **Each channel
in turn**, **Walk around me** and **Loop around me** move a source or you, and **Hold: no room** plays the
same sound without the room. Pick a test in the list, then press Play; picking another one while a test
plays switches to it. Stop lets the tail ring out before closing.

## Code

The engine core is header-only and depends on the standard library: `StageSolver.h` (room acoustics),
`StageTracer.h` (energy rays), `StageMesh.h` (OBJ), `StageBuilder.h` (builds a response),
`StageRenderer.h` (per-block convolvers and taps) and `StageEngine.h` (driver side, background build
thread). `svms_v3_stage_engine`, `svms_v3_stage_renderer` and `svms_v3_stage_gpu` are the tests.
