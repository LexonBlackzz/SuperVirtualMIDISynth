# VirtuallySuper

This folder is the runtime home for the planned `VirtuallySuper` engine.

The engine is still being defined in the design documents under
[../SceneSynth](../SceneSynth/README.md), but Phase 10 has now started with a
small real code slice: a scheduler-only prototype.

## Intent

- keep the new engine physically separate from existing runtime engines
- let implementation grow in modular files from the beginning
- avoid turning the new engine into one giant source file

## Planned Modules

- `VirtuallySuperTypes.*`
- `VirtuallySuperEngine.*`
- `VirtuallySuperConfig.*`
- `VirtuallySuperPools.*`
- `VirtuallySuperScheduler*`
- `VirtuallySuperScene*`
- `VirtuallySuperExact*`
- `VirtuallySuperGrouped*`
- `VirtuallySuperDensity*`
- `VirtuallySuperRender*`
- `VirtuallySuperTelemetry*`
- `VirtuallySuperOverload*`

## Current Phase 10 Slice

The first concrete runtime slice is:

- `VirtuallySuperTypes.h`
- `VirtuallySuperScheduler.h/.cpp`
- `VirtuallySuperScene.h/.cpp`
- `VirtuallySuperExact.h/.cpp`
- `VirtuallySuperGrouped.h/.cpp`
- `VirtuallySuperDensity.h/.cpp`
- `VirtuallySuperSamplerEngine.h/.cpp`
- `VirtuallySuperEngine.h/.cpp`

This slice is intentionally limited:

- fixed-capacity ingress and scheduled-event storage
- per-key transition queues
- deterministic scheduled ordering
- fixed-capacity exact voice pool
- direct `(channel, note)` exact voice lookup
- queue-based exact voice stealing
- window-local grouped bucket prototype
- window-local density cloud prototype
- explicit scene action compiler between scheduler and tiers
- scheduler-to-exact prototype API
- `ISamplerEngine` shell adapter for the current synth code

## Current Integration Status

`VirtuallySuper` now has a runtime shell that plugs into the existing
`ISamplerEngine` abstraction.

Current scope:

- selectable by engine name in source/config as `virtuallysuper`
- event ingress and runtime state reachable through the current synth layer
- diagnostics and active-voice reporting available
- render output still placeholder silence

So the shell is integrated, but the sampler/audio path is not finished yet.

It is not integrated into the existing playback path yet.
