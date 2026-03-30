# Exact Voice System

This document defines the Phase 4 exact voice system design for
`VirtuallySuper`.

The exact voice system is responsible for the fully independent, per-note part
of the engine. It is the tier that protects buzz articulation, foreground note
clarity, and per-note divergence when grouped or density abstractions are not
good enough.

## Goals

- preserve exact per-note playback when required
- keep note-off and retrigger behavior free of global active-voice scans
- support efficient stealing and reuse under pressure
- preserve per-note expressive divergence
- make release shortening and quiet-tail handling explicit and policy-driven

## ExactVoice Responsibilities

Each `ExactVoice` should represent one independently rendered note-layer
instance.

Responsibilities:

- hold literal playback state
- hold note identity and routing identity
- track exact envelope and modulation state
- participate in note-off, release, and steal policy
- remain promotable and demotable relative to grouped or density tiers

## ExactVoice Runtime State

The exact runtime state should be split into hot and cold fields.

Hot fields should include:

- active state flags
- channel
- note
- layer or template ID
- pitch state
- gain state
- envelope state
- render position or sample cursor
- current importance or tier flags
- next lifecycle state

Cold or less-frequently touched fields should include:

- source bank or region identity
- metadata for diagnostics
- creation reason
- promotion history or provenance

The exact bit layout is still an implementation concern, but the separation
between hot and cold state is part of the design.

## Exact Voice Identity

Every exact voice should carry enough identity to support:

- direct note-off lookup
- per-note expression updates
- safe promotion and demotion
- telemetry and debugging

Suggested identity fields:

- `channel`
- `note`
- `generation`
- `layerTemplateId`
- `voiceId`

`generation` is especially important to avoid confusing old and new voices for
the same `(channel, note)` after retriggers or steals.

## Note-Off Direct Lookup Model

The exact system must not use a global active-voice scan for note-off.

Preferred design:

- direct per-key ownership tables for exact voices
- optional per-key head lists or compact exact-voice chains
- per-key generation tracking to distinguish current versus stale instances

The note-off path should only inspect:

- exact voices owned by the relevant `(channel, note)`
- or directly mapped exact voice handles for that key

It must not walk unrelated voices.

## Same-Key Retrigger Handling

Exact voices are the primary protection layer for same-key buzz.

Rules:

- on-time protected retriggers should prefer creating or reusing exact voices
- a retrigger must not accidentally silence a fresh protected attack because an
  older voice for the same key exists
- the exact system should respect scheduler ordering and generation identity

## Steal Structures

Stealing must avoid full active-voice scans.

Required policy structures:

- exact voice free list
- release-candidate structure
- quiet-candidate structure
- active-candidate structure

These may be implemented as:

- heaps
- indexed queues
- bucketed priority lists

The exact structure is not frozen yet, but the design requires bounded,
non-global candidate selection.

## Steal Priority Policy

Stealing should prefer the least musically important exact material first.

Suggested priority order:

1. quiet released exact voices
2. quiet active tails
3. old released exact voices
4. old active exact tails
5. only then more audible material

Protected attacks should strongly resist stealing.

## Release Shortening Policy

Release shortening is a first-class overload tool.

Rules:

- release times may be shortened under pressure
- quiet or masked releases should shorten before exposed material
- release shortening must be telemetry-visible
- exact attacks must not be punished before tails

Release shortening should be driven by:

- overload state
- sustain pressure
- voice equivalent
- exact voice pressure

## Quiet-Tail Culling

Quiet-tail culling is separate from stealing, even if the same data structures
help both.

Good cull candidates:

- quiet releases
- masked sustained tails
- exact voices already marked as demotion-eligible

Poor cull candidates:

- fresh attacks
- newly retriggered buzz notes
- high-importance expressive notes

## Per-Note Expression Escape Path

Exact voices are the destination tier when grouped or density content diverges
into true per-note expression.

Triggers for exact escape:

- unique pitch bend or per-note pressure
- MPE-style note expression
- per-note modulation too divergent for shared grouped behavior

Rules:

- promotion into exact should be explicit
- promoted exact voices should retain provenance for debugging and telemetry
- the system should avoid duplicate exact voices for the same promoted content

## Exact Voice Reuse

The engine should support exact voice reuse where musically safe.

Good reuse cases:

- replacing stale or releasable exact voices
- reusing fully dead exact voice slots

Unsafe reuse cases:

- clobbering protected attacks
- overwriting exact voices still tied to important retrigger sequences

## Exact Voice Pools

Exact voices should live in a fixed-capacity `ExactVoicePool`.

Design rules:

- stable handles or indices
- no allocator calls after startup
- direct per-key mapping should reference pool handles, not heap pointers

## Telemetry Hooks

The exact system should expose cheap counters for:

- active exact voices
- released exact voices
- steals
- shortened releases
- culled quiet tails
- exact promotions from grouped or density

These remain counters or compact aggregates, not UI-formatted output.

## Determinism

Exact voice behavior must remain deterministic in offline mode.

That means:

- stable note-off ownership rules
- stable steal tie-break rules
- stable release shortening decisions
- stable promotion and reuse choices

## Phase 4 Exit Condition

Phase 4 is considered complete when:

- exact voice runtime state is specified
- note-off direct lookup model is specified
- steal heaps or queue strategy is specified at the design level
- release shortening and quiet-tail policy are specified
- per-note expression escape behavior is specified
