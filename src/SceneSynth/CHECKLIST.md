# SceneSynth Checklist

This file is intended to be updated as work lands.

## Already Done

- [x] New engine design folder created
- [x] New runtime engine folder created at `src/VirtuallySuper`
- [x] Initial architecture document written
- [x] Initial subsystem document written
- [x] Runtime foundations document written
- [x] Module map document written
- [x] Event model document written
- [x] Scheduler design document written
- [x] Scene compiler document written
- [x] Exact voice system document written
- [x] Grouped and density rendering document written
- [x] Tile renderer and threading document written
- [x] Diagnostics and live protocol document written
- [x] Initial configurator and diagnostics document written
- [x] Configurator rewrite document written
- [x] Test and validation document written
- [x] Initial staged implementation plan written
- [x] `syndrv.dll` first-pass Ghidra analysis completed
- [x] Engine name chosen: `VirtuallySuper`
- [x] Initial code terminology frozen for planning
- [x] Target platform policy chosen: `x86`, `x64`, `Windows XP`
- [x] Current project already has a portable Configurator baseline
- [x] Current project already has a live diagnostics bridge baseline
- [x] Current project already has multiple existing engines for comparison:
      TSF, BASSMIDI, SFZ

## Open Design Decisions

- [ ] Whether SceneSynth ships alongside TSF first or starts as an internal prototype

## Engine Foundation

- [x] Finalize runtime object model
- [x] Finalize initial file/module split for `VirtuallySuper`
- [x] Finalize layered configuration model
- [x] Finalize memory pool layout
- [ ] Finalize exact/grouped/density accounting model
- [ ] Finalize voice-equivalent definition

## Scheduler

- [x] Fixed-capacity per-key transition queues specified
- [x] Macro-culling rules specified
- [x] Same-key buzz protection rules specified
- [x] Sustain overload rules specified
- [x] Event reduction heuristics specified

## Scene Compiler

- [x] Exact note allocation path specified
- [x] Grouped object allocation path specified
- [x] Density object allocation path specified
- [x] Promotion and demotion rules specified
- [x] Note importance scoring model specified
- [x] Exact attack protection specified

## Exact Voice System

- [x] Exact voice runtime state specified
- [x] Note-off direct lookup model specified
- [x] Steal structure policy specified
- [x] Release shortening and quiet-tail policy specified
- [x] Per-note expression escape path specified

## Grouped And Density Rendering

- [x] Grouped object structure specified
- [x] Grouping keys specified
- [x] Grouped modulation-sharing rules specified
- [x] First density implementation chosen: granular cloud
- [x] Density gain and phase-randomization strategy specified
- [x] Grouped/density handoff rules specified

## Tile Renderer And Threading

- [x] Tile size policy specified
- [x] Job queue format specified
- [x] Worker thread model specified
- [x] Tile merge path specified
- [x] Deterministic offline worker policy specified
- [x] Scalar and SIMD helper strategy specified

## Render

- [x] Exact tier design specified
- [x] Grouped tier design specified
- [x] Density tier design specified
- [x] Tile renderer design specified
- [x] Threading design specified

## Diagnostics

- [x] Shared snapshot schema specified
- [x] Event ring schema specified
- [x] Hot/warm/cold stat groups specified
- [x] Debug level policy specified

## Configurator

- [x] New tab layout finalized
- [x] Basic/advanced/power-user/developer settings split finalized
- [x] SoundFont folder UX finalized
- [x] Debug window finalized
- [x] Graph set finalized
- [x] Profile system finalized

## Validation

- [x] Deterministic offline mode specified
- [x] Audio hash regression strategy specified
- [x] Black MIDI benchmark set specified
- [x] Scheduler fuzz strategy specified

## Prototype And Integration

- [x] First scheduler-only prototype started in `src/VirtuallySuper`
- [x] First exact-tier-only prototype started
- [x] First grouped rendering prototype started
- [x] First density prototype started
- [ ] Prototype telemetry wired for Configurator consumption
