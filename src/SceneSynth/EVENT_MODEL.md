# Event Model

This document defines the normalized internal event model for `VirtuallySuper`.

The purpose of this model is to give the engine one canonical event shape that
all scheduler and scene logic can depend on, regardless of where the event
originated.

## Goals

- fixed-size event representation
- deterministic ordering
- no heap ownership inside hot-path events
- explicit timing and routing fields
- support for future grouped and density scheduling
- future-readiness for per-note expression

## Event Lifecycle

The intended event flow is:

1. external MIDI or host input
2. normalization to `NormalizedEvent`
3. scheduler storage and ordering
4. event reduction and classification
5. scene update

No later stage should need to inspect original raw MIDI bytes.

## Core Event Categories

Planned `EventKind` categories:

- `NoteOn`
- `NoteOff`
- `PolyPressure`
- `ChannelPressure`
- `ControlChange`
- `ProgramChange`
- `PitchBend`
- `SystemReset`
- `AllNotesOff`
- `AllSoundOff`
- `TempoChange`
- `MetaTiming`
- `LongData`
- `UserMarker`

Not every kind needs to survive into every runtime stage, but the normalized
model should be capable of carrying them.

## NormalizedEvent Shape

The engine should use a compact POD-style event shape.

Suggested fields:

- `EventKind kind`
- `uint8_t channel`
- `uint8_t note`
- `uint8_t valueA`
- `uint8_t valueB`
- `uint16_t flags`
- `uint16_t applyPriority`
- `uint32_t sequence`
- `uint32_t sourceTrack`
- `uint32_t sourceTick`
- `int64_t targetSample`
- `uint32_t auxIndex`
- `uint32_t groupHint`

Notes:

- `valueA` and `valueB` are generic compact payload slots for normalized
  controller, pressure, or velocity data
- `auxIndex` is an optional index into side storage for long payloads
- `groupHint` is reserved for grouped or density scheduling assistance

The exact field widths can still shift during implementation, but the data
should remain fixed-size, POD-like, and cheap to copy.

## Timing Fields

Each event should carry both source timing and runtime timing.

Source timing:

- `sourceTrack`
- `sourceTick`
- `sequence`

Runtime timing:

- `targetSample`
- `applyPriority`

Ordering guarantee:

- events are applied in `(targetSample, applyPriority, sequence)` order

This order is the canonical tie-break model for the scheduler.

## Normalization Rules

The following rules should happen before hot scheduler logic:

- `NoteOn` with velocity `0` normalizes to `NoteOff`
- channel range is clamped or rejected to `0..15`
- note range is clamped or rejected to `0..127`
- pitch bend normalizes to a centered signed value or equivalent canonical range
- reset-like events normalize to explicit `SystemReset`, `AllNotesOff`, or
  `AllSoundOff`
- long payloads such as sysex are stored out-of-line behind `auxIndex`

## Event Flags

Useful flag classes:

- exact-attack preferred
- can-be-grouped
- can-be-density
- from-live-input
- from-file-playback
- late-arrival marker
- sustain-sensitive
- developer or marker event

Flags should remain cheap policy hints, not a replacement for scheduler logic.

## Keyed Events

Keyed events are events that target one `(channel, note)` pair.

These include:

- `NoteOn`
- `NoteOff`
- `PolyPressure`

Keyed events are the only events that should participate in per-key
`TransitionQueue` logic.

## Non-Keyed Events

Non-keyed events affect channel, global, or transport state.

These include:

- control changes
- pitch bend
- channel pressure
- program changes
- reset-like messages
- tempo/timing messages

These should bypass per-key queue rules while still participating in canonical
event ordering.

## Long Data

Long data should never inflate the hot event structure itself.

Rules:

- long payloads live in side storage
- hot events carry only `auxIndex`
- hot scheduler logic should avoid touching long payload storage unless required

## Per-Note Expression Readiness

The event model should be future-ready for MPE or MIDI 2 style note-level
expression.

That means:

- keyed events should remain promotable to exact-tier processing
- grouped scheduling must not assume every note on a key shares future
  expressive changes
- future note identity extensions must fit without breaking canonical ordering

## Event Size Rule

The final runtime event should stay small enough for ring-buffer and heap use
without excessive cache cost.

Soft target:

- keep the hot event representation within one cache-friendly fixed-size block
- avoid pointer-heavy or variable-size shapes

## Event Ownership Rule

`NormalizedEvent` must not own runtime heap memory.

Allowed:

- POD payload
- scalar IDs
- compact side-storage indices

Disallowed:

- `std::string`
- `std::vector`
- owning smart pointers
- allocator-dependent internals
