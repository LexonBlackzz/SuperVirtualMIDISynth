# SceneSynth Architecture

This document describes the architecture of the `VirtuallySuper` engine.

## Summary

`SceneSynth` is designed as a three-tier synthesis engine:

1. `Exact Tier`
   Fully independent per-note playback for the most audible material.
2. `Grouped Tier`
   Shared or clustered playback for many similar notes or layers.
3. `Density Tier`
   Perceptual approximation of background note mass when literal playback is no
   longer efficient or musically necessary.

The system should behave like a perceptual audio scene renderer rather than a
traditional one-voice-per-note sampler.

## Primary Objectives

- Preserve fast buzz articulation and same-key retriggers.
- Avoid `O(events * active voices)` behavior in all hot paths.
- Prefer musical degradation over hard failure when overloaded.
- Scale by grouping, abstraction, and scene-level decisions instead of only by
  micro-optimizing a linear sampler loop.
- Keep the live diagnostics path observational and cheap.

## Core Principles

### 1. Zero-Allocation Rule

- Audio thread performs no heap allocation after startup.
- Scheduler thread performs no heap allocation after startup.
- Worker threads perform no heap allocation after startup.
- All pools, rings, heaps, tiles, and telemetry buffers are preallocated.

### 2. Deterministic Hot Paths

- All hot paths use fixed-capacity structures.
- No hot path should rely on `std::map`, linked lists, or unbounded queues.
- Event ordering must be deterministic in offline mode.

### 3. Per-Key Direct State

- Key state is stored as direct tables indexed by `(channel, note)`.
- Same-key transitions are represented as fixed-capacity queues.
- `note_off` and retrigger behavior must not depend on scanning global voice lists.

### 4. Render As Scene, Not Just Voices

- Scheduler classifies events before allocation.
- Not all incoming note events become exact voices.
- The engine may convert heavy note populations into grouped swarms or density
  representations while preserving perceptual intent.

### 5. UI Must Be Decoupled

- The DLL publishes cheap counters and snapshots only.
- The Configurator performs history, graphing, smoothing, text formatting, and
  expensive analysis in its own process and thread.

### 6. Modularity First

- The engine must be split into small, ownership-oriented files rather than a
  single giant implementation unit.
- Each subsystem should have a clear home and a narrow public surface.
- Hot-path code organization should still favor locality, but module boundaries
  must remain understandable for maintenance and review.
- Internal interfaces should be designed so exact, grouped, density, telemetry,
  and Configurator-related work can evolve largely independently.
- The design should prefer "many focused files" over "one mega-engine file"
  unless a measured performance reason forces consolidation.

### 7. Deeply Configurable, Safely Presented

- The engine should be highly configurable by the end-user, including voice,
  layer, grouping, density, overload, and render behavior.
- The default presentation must still remain safe and understandable for normal
  users who only want to change practical controls such as output, latency,
  polyphony, layer count, and SoundFonts.
- Advanced or risky settings should exist, but they should be grouped into
  expert-facing surfaces rather than forced into the basic workflow.
- Runtime hot paths must consume compiled config snapshots, not parse or
  interpret UI-oriented settings directly.
- Configurability should be treated as a first-class product feature, not a
  debugging backdoor.

## Code Terminology

The following names should be treated as the preferred code vocabulary unless
implementation discovers a compelling reason to adjust them:

- `EngineState`
  Top-level runtime state for `VirtuallySuper`.
- `SchedulerState`
  The event ingress, timing, and transition-management subsystem.
- `SceneState`
  The current musical scene after scheduling and reduction.
- `KeyState`
  Direct state for one `(channel, note)` pair.
- `TransitionQueue`
  Fixed-capacity same-key event queue for a `KeyState`.
- `ExactVoice`
  A fully independent per-note playback object.
- `LayerInstance`
  One runtime layer created from a layer template or note plan.
- `GroupedObject`
  A runtime clustered renderer object representing many similar notes or layers.
- `DensityObject`
  A perceptual background-mass renderer object.
- `VoiceEquivalent`
  Aggregate polyphony-style metric spanning exact, grouped, and density tiers.
- `TileJob`
  One fixed-size render job for the worker system.
- `TelemetrySnapshot`
  One cheap published diagnostics snapshot for the Configurator.

These terms are now considered frozen for planning purposes.

## Modularity And File Ownership

`VirtuallySuper` should be implemented as a modular engine with clear ownership
per subsystem.

Suggested high-level layout:

- `src/VirtuallySuper/VirtuallySuperEngine.*`
  Top-level engine lifetime and public integration surface.
- `src/VirtuallySuper/VirtuallySuperScheduler.*`
  MIDI ingress, timing, per-key transitions, and event reduction.
- `src/VirtuallySuper/VirtuallySuperScene.*`
  Scene compilation, tier decisions, and promotion/demotion logic.
- `src/VirtuallySuper/VirtuallySuperExact.*`
  Exact-tier runtime and exact voice policies.
- `src/VirtuallySuper/VirtuallySuperGrouped.*`
  Grouped/swam object logic and grouped rendering policies.
- `src/VirtuallySuper/VirtuallySuperDensity.*`
  Density-tier runtime and perceptual mass rendering.
- `src/VirtuallySuper/VirtuallySuperRender.*`
  Tile generation, worker dispatch, and render orchestration.
- `src/VirtuallySuper/VirtuallySuperTelemetry.*`
  Shared-memory snapshots, debug event ring, and cheap stats publishing.
- `src/VirtuallySuper/VirtuallySuperConfig.*`
  Runtime-facing config translation, validation, presets, and snapshot
  compilation.
- `src/VirtuallySuper/VirtuallySuperTypes.h`
  Shared POD-style runtime types, enums, constants, and IDs.
- `src/VirtuallySuper/VirtuallySuperPools.*`
  Pools, free-lists, and fixed-capacity allocators for real-time use.

This layout is a starting point, not a hard freeze, but the engine should be
planned around subsystem files from the beginning.

## Top-Level Pipeline

1. MIDI ingress
2. event normalization
3. event reduction and classification
4. scene update
5. exact/grouped/density allocation and maintenance
6. tile job generation
7. multi-threaded render
8. output mix, limiter, and diagnostics snapshot

## Tier Definitions

## Exact Tier

Use when:

- note attacks are perceptually important
- same-key buzz articulation must remain exact
- per-note expression diverges
- sustain behavior or foreground clarity matters

Characteristics:

- per-note playback state
- full modulation and routing
- most expensive tier

## Grouped Tier

Use when:

- many notes share the same sample source or layer template
- modulation can be shared or simplified
- exact independence is less important than timing and timbre continuity

Characteristics:

- grouped by sample page, pitch band, channel, layer template, and timing bucket
- shared or clustered modulation
- page-local render batches
- may represent many note events with fewer runtime objects

## Density Tier

Use when:

- event density is too high for literal playback to remain efficient
- background note mass matters more than exact note individuality

Characteristics:

- perceptual approximation, not literal per-note playback
- likely based on granular clouds first, with spectral approaches kept as a
  later research path
- driven by pitch-energy statistics and grouped note density

## Event Reduction Model

The scheduler must decide whether an event becomes:

- `Exact`
- `Grouped`
- `Density`
- `Discarded`

Classification inputs:

- note density on the same key
- channel density
- register importance
- velocity
- event age
- lateness and queue pressure
- sustain pressure
- current render budget
- current voice equivalent
- MPE or per-note divergence

## Overload Ladder

### Soft Pressure

- begin grouping similar notes
- shorten quiet releases
- reduce layer count for less important material

### Hard Pressure

- increase grouped rendering
- begin density substitution for background mass
- kill quiet and masked tails sooner

### Panic Pressure

- reserve exact tier for new attacks and highest-value foreground material
- aggressively collapse or discard stale, weak, or deeply masked events

Important rule:

- over-budget rendering should first reduce tails and abstraction quality
  before dropping fresh note-ons that define articulation.

## Scaling Metrics

The engine should expose multiple polyphony-related metrics:

- `Exact Voices`
- `Layer Instances`
- `Grouped Objects`
- `Voice Equivalent`
- `Density Load`

`Voice Equivalent` becomes the headline metric once the engine can represent
large note populations through grouped or density objects.

## Black MIDI Adaptation

Black MIDI changes the priority order:

- scheduler collapse becomes more important than pure DSP optimization
- event storms must be reduced before full allocation
- same-tick or near-same-tick note floods should become swarm events when
  appropriate
- sustain pedal abuse must be treated as an overload source

The engine should be thought of as a particle system for audio:

- exact tier for perceptually critical particles
- grouped tier for coherent clusters
- density tier for background volumetric mass

## Deterministic Offline Mode

Offline render mode must:

- produce bit-stable output for the same inputs and config
- disable nondeterministic work stealing or time-dependent heuristics
- allow regression hashing and CI-style validation

## MIDI 2.0 / MPE Readiness

Even if initial implementation is MIDI 1.0-focused:

- per-note expressive divergence must be a first-class concept
- grouped notes must be promotable back to exact notes
- exact tier remains the escape hatch when expression becomes too unique to share
