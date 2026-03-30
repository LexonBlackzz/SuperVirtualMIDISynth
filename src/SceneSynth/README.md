# SceneSynth

`SceneSynth` is the planned next-generation sampler engine for SuperVirtualMIDISynth.

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
- [Configurator And Diagnostics](./CONFIGURATOR_AND_DIAGNOSTICS.md)
- [Implementation Plan](./IMPLEMENTATION_PLAN.md)
- [Checklist](./CHECKLIST.md)

## Working Name

`SceneSynth` is a placeholder name chosen to match the planned shift from a
traditional linear sampler into a perceptual audio scene renderer.

It can be renamed later without changing the structure of the design documents.
