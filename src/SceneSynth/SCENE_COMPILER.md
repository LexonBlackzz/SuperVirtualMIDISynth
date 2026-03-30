# Scene Compiler

This document defines the Phase 3 scene compiler design for `VirtuallySuper`.

The scene compiler is the bridge between scheduler output and the actual runtime
objects that the renderer will consume. It decides what becomes exact,
grouped, or density state, and it is where musical importance is translated
into engine-tier behavior.

## Goals

- convert scheduler-approved events into scene actions
- protect perceptually important attacks and retriggers
- keep exact, grouped, and density tiers logically separate
- make promotion and demotion explicit rather than accidental
- prepare render-facing buckets and jobs without forcing the scheduler to know
  render details

## Position In The Pipeline

The scene compiler runs after:

1. normalization
2. ordering
3. same-key queue protection
4. event reduction and admission

The scene compiler runs before:

1. exact-tier state updates
2. grouped-tier state updates
3. density-tier state updates
4. tile job generation

It should be treated as a scene update translator, not as the renderer itself.

## Inputs

The scene compiler consumes:

- ordered scheduler outputs
- `SchedulerState` pressure metrics
- current `SceneState`
- `RuntimeConfigSnapshot`
- current per-channel and per-key applied state

## Outputs

The scene compiler should emit scene-facing actions such as:

- `SpawnExactVoice`
- `ReleaseExactVoice`
- `RetargetExactVoice`
- `SpawnGroupedObject`
- `UpdateGroupedObject`
- `RetireGroupedObject`
- `SpawnDensityObject`
- `UpdateDensityObject`
- `RetireDensityObject`
- `DiscardEvent`

The exact names may change in code, but the action model should remain explicit.

## Exact Note Allocation Path

The exact path is for events that must remain individually audible.

Typical triggers:

- fresh foreground attacks
- exact same-key buzz retriggers
- per-note expressive divergence
- notes that are too important to collapse

Exact path responsibilities:

- allocate or reuse `ExactVoice` state
- bind note identity to exact playback
- preserve attack timing and note individuality
- keep exact notes promotable from grouped or density tiers when needed

Allocation decision inputs:

- note importance score
- current exact voice budget
- whether the event is a protected attack
- whether the note already exists in exact form
- MPE or per-note modulation divergence

## Grouped Object Allocation Path

The grouped path is for note populations that remain musically relevant but do
not need full per-note runtime cost.

Typical triggers:

- repeated or clustered notes with similar source/sample family
- many notes in a common pitch band or timing bucket
- material that benefits from page-local or SIMD-friendly batching

Grouped path responsibilities:

- create or update grouped swarm objects
- preserve timing and pitch identity at the cluster level
- carry enough metadata for later promotion back to exact notes if required
- retain a represented-note count for accounting and telemetry

Suggested grouping keys:

- channel
- sample family or page group
- pitch band
- layer template
- timing bucket

## Density Object Allocation Path

The density path is for background note mass where literal or grouped playback
is no longer worthwhile.

Typical triggers:

- extreme note floods
- heavy background sustain walls
- material that is perceptually part of a mass rather than an individual line

Density path responsibilities:

- create or update density objects driven by note-energy statistics
- keep output musically useful rather than numerically literal
- preserve register, density, and attack energy at a perceptual level

Initial density path should remain abstract at this phase. The first density
implementation choice is still an open design decision.

## Promotion And Demotion Rules

The scene compiler must control movement between tiers explicitly.

### Promotion

Promotion moves material toward a more exact representation.

Typical promotion causes:

- per-note expression divergence
- newly important foreground attacks
- scheduler exact-attack protection
- grouped or density content becoming sparse enough to justify exact playback

Promotion examples:

- grouped note cluster -> exact note attack
- density contribution -> grouped object
- grouped object member -> exact note

### Demotion

Demotion moves material toward a cheaper representation.

Typical demotion causes:

- sustained background tails
- overload pressure
- perceptual masking
- density growth beyond exact-tier budget

Demotion examples:

- exact tail -> grouped tail object
- grouped mass -> density contribution

## Note Importance Scoring

The scene compiler should use an explicit importance score rather than scattered
hard-coded special cases.

Inputs to importance scoring should include:

- velocity
- register importance
- freshness of attack
- same-key retrigger protection
- channel prominence
- per-note expression divergence
- current overload pressure
- sustain pressure
- masking risk

The exact numeric formula does not need to be frozen yet, but the existence of
an importance-driven decision model is required.

## Exact Attack Protection

Exact attack protection is a top-level rule.

Protected attacks are events that should resist collapse or shedding unless the
engine is already in extreme failure territory.

Protected attack examples:

- fresh note-on edges
- exact same-key buzz retriggers that are on time
- newly promoted expressive notes

Rules:

- attack protection outranks broad tail preservation
- attack protection should be consulted before demoting exact material
- overload should first demote tails and background mass before discarding
  protected attacks

## Tail Fusion Eligibility

The scene compiler decides when tails may be fused into grouped or density
representations.

Good tail-fusion candidates:

- quiet releases
- masked older notes
- sustained background material

Poor tail-fusion candidates:

- fresh attacks
- exposed melodic notes
- highly divergent expressive notes

## Per-Channel Layer Reduction

Layer reduction should be a scene compiler decision rather than a hidden
renderer side effect.

Rules:

- keep layer reduction explicit and telemetry-visible
- prefer reducing less important background layers first
- allow exact foreground attacks to retain fuller layer plans when possible

## Scene Buckets

The compiler should prepare the scene in a render-friendly shape.

Suggested bucket dimensions:

- exact buckets
- grouped buckets
- density buckets
- sample-page hints
- pitch-band hints
- per-channel slices

The compiler prepares these buckets; the tile renderer later turns them into
actual `TileJob`s.

## Determinism

Scene compiler behavior must remain deterministic in offline mode.

That means:

- stable promotion and demotion rules
- stable scoring tie-break behavior
- stable exact/grouped/density assignment for identical inputs
- stable layer reduction behavior

## Phase 3 Exit Condition

Phase 3 is considered complete when:

- exact note allocation path is specified
- grouped object allocation path is specified
- density object allocation path is specified
- promotion and demotion rules are specified
- note importance scoring exists as a design concept
- exact attack protection is specified
