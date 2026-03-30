# Scheduler Design

This document defines the Phase 2 scheduler design for `VirtuallySuper`.

The scheduler is the most critical part of the engine for Black MIDI survival,
same-key buzz articulation, and general scalability. It should be treated as a
scene compiler front-end, not just a queue of raw MIDI messages.

## Goals

- preserve exact same-key retriggers when musically important
- avoid `O(events * active voices)` behavior
- reduce event floods before full voice or layer allocation
- support exact, grouped, density, and discarded outcomes
- degrade musically under pressure instead of flattening fresh attacks

## Scheduler Pipeline

The intended scheduler pipeline is:

1. ingress
2. normalization
3. far-future scheduling
4. render-window drain
5. same-key queue processing
6. event reduction and admission
7. scene update emission

Each stage should operate on fixed-capacity structures.

## Ingress Rings

The scheduler should use fixed-capacity rings for realtime-safe event ingress.

Suggested lanes:

- `CriticalIngressRing`
  Reset-like and must-apply control traffic.
- `RealtimeIngressRing`
  General live MIDI traffic.
- `DenseNoteIngressRing`
  Note-heavy or burst-heavy traffic that may require separate handling.

The exact lane count can still change, but the important rule is:

- no linked-list ingress
- no unbounded queue growth
- no hot-path allocation

## Far-Future Scheduling

Events outside the current render window should live in a deterministic
far-future structure.

Recommended first design:

- min-heap keyed by `(targetSample, applyPriority, sequence)`

Later upgrades may use:

- calendar queues
- sample-window buckets
- hybrid heap and bucket designs

But the first version should stay simple and deterministic.

## Per-Key Transition Queues

Each `(channel, note)` pair should own a fixed-capacity `TransitionQueue`.

Responsibilities:

- preserve same-key alternation order
- prevent accidental collapse of legitimate buzz retriggers
- track pending same-key note state within the scheduling window

Rules:

- exact duplicate same-state events at the same sample may coalesce
- distinct alternating note-on and note-off transitions must remain separate
- opposite-state events at different times must not be collapsed away just
  because the key is already represented

Recommended initial capacity:

- normal builds: `8` pending transitions per key
- legacy-conservative option: `4` for tighter old-system memory limits

## Render-Window Drain

Near-term events should be drained into the current render window in canonical
order before reduction.

Suggested approach:

- drain from far-future structure into window-local batches
- maintain `(targetSample, applyPriority, sequence)` order
- prepare keyed events for `TransitionQueue` processing

Optional future refinement:

- sub-buckets inside the render window for exact sample or sub-block handling

## Same-Key Buzz Protection

This is a core feature, not an optimization detail.

Rules:

- same-key rapid alternations must remain representable
- exact buzz retriggers should be preserved whenever on-time and not already
  stale
- overload handling should not default to "latest event wins" for the same key
- exact attack protection outranks broad note flattening

## Event Reduction Outcomes

After ordering and same-key handling, each candidate event should become one of:

- `Exact`
- `Grouped`
- `Density`
- `Discarded`

This decision should happen before full scene allocation.

## Event Reduction Inputs

Reduction decisions should consider:

- channel and key density
- velocity
- register importance
- freshness or staleness
- sustain pressure
- queue age
- lateness
- render budget pressure
- current exact voices
- current voice equivalent

## Macro-Culling

Black MIDI floods require event-level reduction before full allocation.

Macro-culling should support:

- collapsing same-key floods at the same or nearly the same time
- collapsing near-identical note populations into grouped swarm candidates
- producing density candidates instead of thousands of literal note-ons when
  the scene is already beyond exact-tier capacity

The scheduler should think in terms of:

- note attacks that must survive
- note populations that can be represented statistically

## Grouping Hints

The scheduler may attach grouping hints before the scene compiler runs.

Useful grouping hints:

- sample page family
- pitch band
- channel
- timing bucket
- layer template

These should stay hints, not final binding decisions.

## Sustain Pressure Policy

Sustain should be treated as a first-class pressure source.

Rules:

- very high sustain pressure can justify aggressive tail aging
- old sustained tails may be demoted before fresh attacks are sacrificed
- the scheduler should track sustain-driven backlog separately from ordinary
  note density when possible

## Late Pressure Policy

The scheduler should distinguish:

- high density but still on-time
- true lateness

Only true lateness should strongly justify destructive due-event shedding.

This distinction is critical for keeping buzz articulation alive on dense but
still-timely passages.

## Window-Local Priorities

Within one render window, event priorities should roughly follow:

1. critical reset/control traffic
2. exact note attacks
3. exact same-key alternations already in flight
4. grouped candidates
5. density candidates
6. stale or weak discard candidates

This is a design guide, not a rigid API contract.

## Scheduler Output To Scene

The scheduler should not allocate full voices directly.

It should emit scene-facing actions such as:

- spawn exact note
- update exact note
- create grouped swarm candidate
- update grouped density contribution
- discard stale event

This keeps scheduler logic and scene logic separate and modular.

## Determinism Rules

Scheduler determinism requires:

- stable tie-break order
- stable queue drain order
- stable reduction order
- stable same-key queue behavior
- seeded randomness only where explicitly intended and disabled or fixed in
  offline deterministic mode

## Phase 2 Exit Condition

Phase 2 is considered complete when:

- the normalized event format is frozen at the design level
- ingress, far-future, and per-key scheduler structures are specified
- same-key buzz protection rules are specified
- macro-culling rules are specified
- sustain and late-pressure behavior are specified
- the scene compiler can be designed against scheduler outputs instead of raw
  MIDI events
