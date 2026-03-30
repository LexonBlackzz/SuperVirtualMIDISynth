# SceneSynth Implementation Plan

This plan is intentionally staged so the project can keep shipping the current
engines while the new engine is designed and prototyped in parallel.

## Phase 0: Design Freeze And Guardrails

- [x] Create a dedicated design home under `src/SceneSynth`
- [x] Write initial architecture documents
- [x] Capture Ghidra observations from `syndrv.dll`
- [x] Agree on the engine name: `VirtuallySuper`
- [x] Freeze baseline terminology for code planning: `EngineState`,
      `SchedulerState`, `SceneState`, `KeyState`, `TransitionQueue`,
      `ExactVoice`, `LayerInstance`, `GroupedObject`, `DensityObject`,
      `VoiceEquivalent`, `TileJob`, `TelemetrySnapshot`
- [x] Freeze target platform policy: `x86`, `x64`, and `Windows XP`
      compatibility for the legacy-supported build path

## Phase 1: Runtime Foundations

- [x] Define `EngineState`, `SchedulerState`, `SceneState`, and `TelemetryState`
- [x] Define the initial `VirtuallySuper` file and module map
- [x] Decide which shared types live in `VirtuallySuperTypes.h`
- [x] Decide which helpers stay header-only and which belong in `.cpp` units
- [x] Define the layered configuration model and runtime snapshot approach
- [x] Define zero-allocation memory policy for all real-time threads
- [x] Define object pools and free-list strategy
- [x] Define hot memory layout and page-locking policy where supported
- [x] Define deterministic offline mode requirements
- [x] Create the initial `src/VirtuallySuper/` source home

## Phase 2: Event Ingress And Scheduler

- [x] Define normalized MIDI event format
- [x] Define fixed-capacity ingress rings
- [x] Define far-future scheduling structure
- [x] Define per-key same-key transition queues
- [x] Define event reduction rules for exact/grouped/density classification
- [x] Define sustain-pressure policy
- [x] Define Black MIDI macro-culling rules

## Phase 3: Scene Compiler

- [ ] Define exact note allocation path
- [ ] Define grouped object allocation path
- [ ] Define density object allocation path
- [ ] Define promotion and demotion rules between tiers
- [ ] Define note importance scoring
- [ ] Define exact attack protection policy

## Phase 4: Exact Voice System

- [ ] Define exact voice runtime state
- [ ] Define note-off direct lookup model
- [ ] Define steal heaps or queues
- [ ] Define release shortening and quiet-tail culling policy
- [ ] Define per-note expression escape path

## Phase 5: Grouped And Density Rendering

- [ ] Define grouped render object structure
- [ ] Define grouping keys: page, pitch band, layer template, timing bucket
- [ ] Define grouped modulation-sharing rules
- [ ] Choose first density approach: likely granular cloud
- [ ] Define density gain and phase randomization strategy
- [ ] Define handoff rules between grouped and density tiers

## Phase 6: Tile Renderer And Threading

- [ ] Define tile size policy
- [ ] Define job queue format
- [ ] Define worker thread model
- [ ] Define tile merge path
- [ ] Define deterministic worker policy for offline mode
- [ ] Define scalar and SIMD render helper strategy

## Phase 7: Diagnostics And Live Protocol

- [ ] Define hot, warm, and cold stat groups
- [ ] Define shared-memory snapshot format
- [ ] Define SPSC debug event ring
- [ ] Define stat update cadence
- [ ] Define debug level policy

## Phase 8: Configurator Rewrite

- [ ] Define tab structure
- [ ] Define basic, advanced, expert, and developer setting surfaces
- [ ] Define SoundFont discovery UX
- [ ] Define diagnostics graphs and views
- [ ] Define profile import/export format
- [ ] Define advanced and developer modes
- [ ] Define versioning strategy for protocol changes

## Phase 9: Test And Validation Infrastructure

- [ ] Define deterministic audio hash tests
- [ ] Define Black MIDI stress corpus
- [ ] Define fuzzing strategy for scheduler and MIDI parser
- [ ] Define live diagnostics regression checks
- [ ] Define performance benchmark harness

## Phase 10: Prototype And Integration

- [ ] Build first scheduler-only prototype
- [ ] Build first exact-tier-only prototype
- [ ] Add grouped rendering prototype
- [ ] Add first density prototype
- [ ] Wire Configurator to prototype telemetry
- [ ] Decide when the new engine is ready for side-by-side runtime integration

## Exit Criteria For First Public Prototype

- [ ] Stable buzz articulation at high retrigger rates
- [ ] Graceful behavior beyond the current TSF ceiling
- [ ] Configurator can observe the engine without measurable disruption
- [ ] Offline deterministic test pass exists
- [ ] No hot-path heap allocation after startup
