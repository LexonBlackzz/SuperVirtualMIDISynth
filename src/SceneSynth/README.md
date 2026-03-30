# SceneSynth

`VirtuallySuper` is the planned next-generation sampler engine for
SuperVirtualMIDISynth.

It is intentionally being designed as a new engine instead of an incremental `tsf.h`
rewrite. The target is not just "more voices," but better musical behavior under
extreme event density, Black MIDI workloads, and future grouped/density rendering.

This folder currently contains design documents only. No runtime code lives here yet.

## Design Goals

- Portable, self-contained DLL with no required external synth runtime.
- Zero-allocation audio and scheduling threads after startup.
- Accurate same-key buzz and retrigger behavior.
- Strong graceful degradation under overload.
- Scalable architecture for exact voices, grouped swarms, and density rendering.
- Cheap live diagnostics with expensive visualization work pushed to the Configurator.
- Deterministic offline render mode for testing.

## Document Map

- [Architecture](./ARCHITECTURE.md)
- [Subsystems](./SUBSYSTEMS.md)
- [Runtime Foundations](./RUNTIME_FOUNDATIONS.md)
- [Module Map](./MODULE_MAP.md)
- [Event Model](./EVENT_MODEL.md)
- [Scheduler Design](./SCHEDULER_DESIGN.md)
- [Scene Compiler](./SCENE_COMPILER.md)
- [Exact Voice System](./EXACT_VOICE_SYSTEM.md)
- [Grouped And Density Rendering](./GROUPED_AND_DENSITY_RENDERING.md)
- [Tile Renderer And Threading](./TILE_RENDERER_AND_THREADING.md)
- [Diagnostics And Live Protocol](./DIAGNOSTICS_AND_LIVE_PROTOCOL.md)
- [Configurator And Diagnostics](./CONFIGURATOR_AND_DIAGNOSTICS.md)
- [Implementation Plan](./IMPLEMENTATION_PLAN.md)
- [Checklist](./CHECKLIST.md)

## Engine Name

The engine name is `VirtuallySuper`.

The folder name `SceneSynth` is retained for the design workspace because it
already groups the architecture documents cleanly. Runtime code can adopt the
`VirtuallySuper` naming directly when implementation begins.

## Platform Targets

The initial target matrix for `VirtuallySuper` is:

- `x86`
- `x64`
- `Windows XP` compatibility for the supported legacy build path

`NT 4` is out of scope for the initial design.
