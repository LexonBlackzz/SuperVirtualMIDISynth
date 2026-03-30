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
- `VirtuallySuperEngine.h/.cpp`

This slice is intentionally limited:

- fixed-capacity ingress and scheduled-event storage
- per-key transition queues
- deterministic scheduled ordering
- scheduler-only prototype API

It is not integrated into the existing playback path yet.
