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
- [x] Initial configurator and diagnostics document written
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

- [ ] First density implementation choice
- [ ] Whether SceneSynth ships alongside TSF first or starts as an internal prototype
- [ ] Whether grouped rendering is per-channel, per-bucket, or hybrid by default

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
- [ ] Basic/advanced/expert/developer settings split finalized
- [ ] SoundFont folder UX finalized
- [ ] Debug window finalized
- [ ] Graph set finalized
- [ ] Profile system finalized

## Validation

- [ ] Deterministic offline mode specified
- [ ] Audio hash regression strategy specified
- [ ] Black MIDI benchmark set specified
- [ ] Scheduler fuzz strategy specified
