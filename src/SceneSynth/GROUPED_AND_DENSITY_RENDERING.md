# Grouped And Density Rendering

This document defines the Phase 5 grouped and density rendering design for
`VirtuallySuper`.

The grouped and density tiers are the core scalability mechanisms that let the
engine move beyond a purely literal per-voice sampler. Grouped rendering keeps
many similar notes musically present at lower cost, while density rendering
handles background note mass when even grouped playback would be wasteful.

## Goals

- represent many similar notes with fewer runtime objects
- preserve timing and pitch identity where it still matters
- keep grouped work cache-friendly and SIMD-friendly
- treat density rendering as a controlled perceptual approximation, not a
  failure mode
- keep handoff rules explicit between exact, grouped, and density tiers

## Grouped Rendering Strategy

The default grouped rendering strategy should be `hybrid`.

That means grouped objects are keyed by more than one dimension instead of
being purely per-channel or purely per-bucket.

The initial grouped dimensions should include:

- channel
- sample page or sample family
- pitch band
- layer template
- timing bucket

This resolves the earlier design question of whether grouped rendering should
default to per-channel, per-bucket, or hybrid: the answer is `hybrid`.

## GroupedObject Structure

Each `GroupedObject` should represent a coherent cluster of notes or layers that
can safely share some runtime work.

Suggested grouped-object fields:

- `groupId`
- `channel`
- `sampleFamilyId`
- `pageGroupId`
- `pitchBandId`
- `layerTemplateId`
- `timingBucketId`
- `representedNoteCount`
- `representedLayerCount`
- `importanceAggregate`
- `energyAggregate`
- `gainState`
- `renderBucketId`
- `groupFlags`

Hot grouped state should remain compact and oriented toward rendering.

Cold grouped state can include:

- provenance/debug metadata
- promotion history
- derived statistics not needed on every block

## Grouping Keys

The initial grouping keys are:

- `sample page`
- `pitch band`
- `layer template`
- `timing bucket`
- `channel`

These are enough to create musically meaningful and cache-friendly grouped
objects without needing the full literal identity of every member note.

### Sample Page

This key improves locality and makes sample fetch behavior friendlier to cache
and SIMD batching.

### Pitch Band

This key prevents grouped objects from spanning notes that are too far apart
musically or spectrally.

### Layer Template

This key prevents unrelated SoundFont or preset layer logic from being forced
into one grouped object.

### Timing Bucket

This key preserves rhythmic coherence and keeps attacks that occur far apart
from being collapsed incorrectly.

### Channel

This key keeps channel-specific controls and routing behavior tractable.

## Grouped Modulation-Sharing Rules

Grouped objects may share or simplify modulation, but only within limits.

Safe candidates for shared behavior:

- channel-level controls
- slowly changing gain shaping
- shared filter tendencies for similar material
- shared page-local render constants

Unsafe or promotion-worthy divergence:

- strong per-note pitch divergence
- highly exposed envelope differences
- per-note expression or MPE-style changes
- attacks whose individuality is musically critical

Rule:

- grouped objects may share modulation only while the shared result remains a
  good perceptual approximation

## Grouped Rendering Responsibilities

Grouped rendering should:

- preserve cluster-level timing
- preserve pitch region identity
- remain friendly to page-local batching
- expose represented-note and represented-layer counts for telemetry
- remain promotable back to exact voices when needed

## First Density Implementation Choice

The first density implementation choice is:

- `granular cloud`

This resolves the earlier open question about the first density approach.

Reasons:

- more naturally tied to sample-based playback than pure spectral synthesis
- better fit for preserving SoundFont character
- easier to phase-jitter and decorrelate than gigantic literal note stacks
- more practical as a first implementation than a full spectral mass renderer

## DensityObject Structure

Each `DensityObject` should represent a background mass of note energy rather
than a literal note cluster.

Suggested density-object fields:

- `densityId`
- `channelMask` or dominant channel
- `pitchBandId`
- `sampleFamilyId`
- `energyLevel`
- `representedNoteCount`
- `representedLayerCount`
- `grainProfileId`
- `spreadState`
- `densityFlags`

## Granular Cloud Responsibilities

The first density implementation should:

- pull micro-grains from the relevant sample family
- preserve broad pitch identity
- preserve attack energy in a mass-safe way
- avoid metallic phase lock and brutal clipping
- degrade into a musically useful cloud instead of a flat distorted sum

## Density Gain Strategy

Density gain should not scale linearly with represented note count.

Required behavior:

- use a saturating or logarithmic gain model
- preserve punch without hard flatlining under extreme density
- keep density output under control before the limiter is forced to do all the
  work

In other words:

- `1000` represented notes should not imply `1000x` gain

## Phase Randomization And Jitter

Density rendering must avoid hard phase alignment.

Required strategies:

- randomized grain start offsets
- microscopic phase spread
- small playback-position jitter where musically safe
- optional tiny decorrelation in rate or stereo spread

Goals:

- avoid metallic flanging
- avoid pathological constructive interference
- maintain a broad, believable wall-of-sound texture

## Handoff Rules Between Grouped And Density Tiers

Grouped and density tiers should not be isolated silos. They need explicit
handoff rules.

### Grouped -> Density

Typical causes:

- represented note count grows too large
- grouped object becomes background mass
- overload pressure makes grouped precision too expensive

### Density -> Grouped

Typical causes:

- the mass thins out enough to justify more structure
- timing or pitch coherence becomes newly important
- the density object becomes too sparse to justify remaining a cloud

### Exact -> Grouped

Typical causes:

- old tails become demotion-eligible
- overload pressure increases
- material becomes masked enough to lose exact priority

### Grouped -> Exact

Typical causes:

- protected attacks
- per-note divergence
- newly important foreground material

## Telemetry Requirements

Grouped and density tiers should expose:

- active grouped object count
- active density object count
- represented note count
- represented layer count
- grouped-to-density demotions
- density-to-grouped promotions
- density gain reductions
- phase-randomization activity counters if cheap enough

## Determinism

Grouped rendering and density rendering must remain deterministic in offline
mode.

That means:

- stable grouping keys
- stable assignment to grouped objects
- stable density handoff decisions
- seeded and fixed randomness for granular behavior in offline mode

## Phase 5 Exit Condition

Phase 5 is considered complete when:

- grouped render object structure is specified
- grouping keys are specified
- grouped modulation-sharing rules are specified
- the first density approach is chosen
- density gain and phase-randomization strategy are specified
- handoff rules between grouped and density tiers are specified
