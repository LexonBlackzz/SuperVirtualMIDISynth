# SceneSynth Checklist

This file is intended to be updated as work lands.

## Already Done

- [x] New engine design folder created
- [x] Initial architecture document written
- [x] Initial subsystem document written
- [x] Initial configurator and diagnostics document written
- [x] Initial staged implementation plan written
- [x] `syndrv.dll` first-pass Ghidra analysis completed
- [x] Current project already has a portable Configurator baseline
- [x] Current project already has a live diagnostics bridge baseline
- [x] Current project already has multiple existing engines for comparison:
      TSF, BASSMIDI, SFZ

## Open Design Decisions

- [ ] Final engine name
- [ ] First density implementation choice
- [ ] XP support policy for the new engine
- [ ] Whether SceneSynth ships alongside TSF first or starts as an internal prototype
- [ ] Whether grouped rendering is per-channel, per-bucket, or hybrid by default

## Engine Foundation

- [ ] Finalize runtime object model
- [ ] Finalize memory pool layout
- [ ] Finalize exact/grouped/density accounting model
- [ ] Finalize voice-equivalent definition

## Scheduler

- [ ] Fixed-capacity per-key transition queues specified
- [ ] Macro-culling rules specified
- [ ] Same-key buzz protection rules specified
- [ ] Sustain overload rules specified
- [ ] Event reduction heuristics specified

## Render

- [ ] Exact tier design specified
- [ ] Grouped tier design specified
- [ ] Density tier design specified
- [ ] Tile renderer design specified
- [ ] Threading design specified

## Diagnostics

- [ ] Shared snapshot schema specified
- [ ] Event ring schema specified
- [ ] Hot/warm/cold stat groups specified
- [ ] Debug level policy specified

## Configurator

- [ ] New tab layout finalized
- [ ] SoundFont folder UX finalized
- [ ] Debug window finalized
- [ ] Graph set finalized
- [ ] Profile system finalized

## Validation

- [ ] Deterministic offline mode specified
- [ ] Audio hash regression strategy specified
- [ ] Black MIDI benchmark set specified
- [ ] Scheduler fuzz strategy specified
