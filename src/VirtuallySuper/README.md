# VirtuallySuper

This folder is the runtime home for the planned `VirtuallySuper` engine.

At the current stage it is a scaffold only. The engine is still being defined in
the design documents under [../SceneSynth](../SceneSynth/README.md).

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

No runtime code is integrated yet. This folder exists so Phase 1 has a real
source root and a stable place to grow from.
