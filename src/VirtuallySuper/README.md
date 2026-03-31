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
- `VirtuallySuperOverload.h/.cpp`
- `VirtuallySuperRender.h/.cpp`
- `VirtuallySuperSoundFontTypes.h`
- `VirtuallySuperSoundFontParser.h/.cpp`
- `VirtuallySuperSoundFontDispatch.h/.cpp`
- `VirtuallySuperSoundFontRuntime.h/.cpp`
- `VirtuallySuperTelemetryShared.h`
- `VirtuallySuperTelemetry.h/.cpp`
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
- overload-aware scene tier selection
- explicit scene action compiler between scheduler and tiers
- compact prototype telemetry snapshots
- scheduler-to-exact prototype API
- first tile-oriented prototype render backbone
- `ISamplerEngine` shell adapter for the current synth code

## Current Integration Status

`VirtuallySuper` now has a runtime shell that plugs into the existing
`ISamplerEngine` abstraction.

Current scope:

- selectable by engine name in source/config as `virtuallysuper`
- included in the normal `x86` and `x64` MSVC DLL builds
- event ingress and runtime state reachable through the current synth layer
- diagnostics and active-voice reporting available through the existing live
  bridge
- current Configurator can display minimal `VirtuallySuper` summary data
- separate `Configurator V2` shell can attach and display compact live
  diagnostics
- deterministic prototype audio rendering available
- native cleanroom `.sf2` ingestion available for exact-tier playback
- exact tier now renders real sample-backed voices for `.sf2` sources
- grouped and density tiers remain prototype-synthetic for now
- `.sf2` load failure is explicit; there is no silent fallback when a real
  SoundFont was requested

So the shell is integrated into the current playback path and `VirtuallySuper`
now has a first real native sampler path for exact voices, but grouped and
density SoundFont playback are still future work.
