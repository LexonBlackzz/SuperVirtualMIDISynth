# SceneSynth Test And Validation

## Phase Intent

`VirtuallySuper` is too ambitious to validate by ear alone.

The engine must be testable in ways that cover:

- deterministic correctness
- musical articulation
- overload behavior
- scheduler stability
- telemetry regressions
- performance scaling

Phase 9 defines the validation system that should exist before the engine is
treated as production-ready.

## Validation Principles

- correctness must not rely only on live listening
- performance claims must come from repeatable benchmarks
- overload behavior must be tested intentionally, not discovered accidentally
- diagnostics must be validated as data contracts, not just UI output
- offline deterministic mode is the foundation for serious regression tests

## Test Layers

The validation strategy should be split into five layers:

- unit-style subsystem validation
- deterministic offline render validation
- scheduler and parser fuzzing
- live diagnostics regression checks
- performance benchmark harnesses

## Deterministic Offline Validation

Offline mode must support repeatable rendering with:

- fixed worker policy
- deterministic event order
- deterministic scene decisions
- stable random seeds for grouped and density behavior
- stable profile/config inputs

### Requirements

- same MIDI + same SoundFont set + same config + same engine build must produce
  the same output audio bytes in offline deterministic mode
- grouped and density tiers must use seeded deterministic randomness
- worker scheduling in offline mode must not leak nondeterminism into output

### Output Strategy

Preferred output form:

- `32-bit float WAV`

Optional additional forms:

- raw interleaved float dump
- hash-only mode for CI

## Audio Hash Regression Tests

The baseline regression method should be audio hashing.

### Strategy

For each test case:

- load a known MIDI
- load a known SoundFont set
- load a known profile/config
- render offline deterministically
- compute a stable hash of the audio output
- compare with the expected reference hash

### Hash Scope

Recommended:

- hash the PCM payload only
- exclude WAV metadata fields that may vary

### Failure Policy

Hash mismatch means one of:

- regression
- intentional algorithmic change
- changed test assets

Every mismatch must be reviewed and explicitly re-baselined if intentional.

## Golden Test Corpus

The project should maintain a curated offline corpus with named intent.

### Core Musical Cases

- single-note pitch sanity
- same-key retrigger articulation
- drum/exclusive-group behavior
- sustain-heavy passages
- release-tail behavior
- velocity layering behavior
- stereo image sanity

### Black MIDI Cases

- dense same-key buzz at `100 Hz`
- dense same-key buzz at `200 Hz`
- dense same-key buzz at `400 Hz`
- mixed-register dense buzz
- sustain-pedal flood
- note-on tsunami with limited note-off density
- repeated reset/restart stress

### Tier Transition Cases

- exact-only baseline
- exact to grouped transition
- grouped to density transition
- density recovery back toward grouped/exact

## Scheduler And MIDI Parser Fuzzing

The scheduler must be attacked deliberately.

### Fuzz Targets

- MIDI parser
- normalized event ingress
- future event scheduling
- same-key transition queues
- sustain and reset interactions
- overload ladder transitions

### Fuzz Inputs

- malformed MIDI streams
- contradictory CC sequences
- huge note-on bursts
- out-of-order event timestamps
- repeated same-key alternations
- repeated reset and all-notes-off storms

### Success Criteria

- no crash
- no memory corruption
- no deadlock
- deterministic failure handling where expected
- scheduler remains internally consistent

## Live Diagnostics Regression Checks

Diagnostics must be validated as a protocol contract.

### Snapshot Validation

Check:

- structure version
- sequence behavior
- hot/warm/cold cadence expectations
- field ranges and monotonic counters
- no impossible negative/overflowed values

### Event Ring Validation

Check:

- ring overrun behavior is understood and surfaced
- event ordering remains producer-stable
- disabled debug modes do not emit heavy traffic

### UI-Visible Contract Checks

Verify:

- protocol mismatch is surfaced clearly
- monitoring-only fallback behavior is correct
- unsafe controls disable themselves when versions differ

## Performance Benchmark Harness

Performance validation must be first-class.

### Harness Goals

- repeatable render timing
- clear workload naming
- side-by-side profile comparison
- easy comparison against current engines

### Core Metrics

- render ms per block
- peak render ms
- average render ms
- exact voices
- layer instances
- grouped objects
- voice equivalent
- steals
- culls
- event collapse rate
- late events
- queue depth

### Benchmark Modes

- offline deterministic benchmark
- live real-time benchmark
- diagnostics-on benchmark
- diagnostics-off benchmark

### Baseline Workloads

- `500` steady voices
- `1000` steady voices
- `2000` steady voices
- `5000` overload stress
- same-key buzz at `100`, `200`, `400 Hz`
- mixed dense chopped buzz
- sustain flood
- reset/restart stress

### Comparison Targets

When practical, compare against:

- current TSF path
- current BASSMIDI path
- current SFZ path

The benchmark harness should make it easy to say:

- what workload was run
- with what config/profile
- on what machine
- with what result

## Bench Result Recording

Benchmark output should be both machine-readable and human-readable.

Preferred outputs:

- `json` summary
- optional text report

Each run should record:

- engine name
- build type
- architecture (`x86` / `x64`)
- deterministic/live mode
- profile name
- MIDI asset ID
- SoundFont set ID
- machine info summary
- result metrics

## Stress Escalation Ladder

Validation should escalate intentionally.

### Level 1

- correctness-focused small tests

### Level 2

- normal musical density

### Level 3

- heavy exact-tier stress

### Level 4

- grouped/density transition stress

### Level 5

- Black MIDI apocalypse workloads

The engine does not need to sound perfect at Level 5.
It does need to fail gracefully and predictably.

## CI Direction

The project should support a headless validation path suitable for automation.

### Recommended CI Jobs

- build matrix:
  - `x86` with `MSVC`
  - `x64` with `MSVC`
  - XP-compatible path with legacy `MinGW`
- offline deterministic render tests
- audio hash checks
- protocol schema checks
- scheduler fuzz smoke tests

Longer Black MIDI stress runs may stay outside fast CI and run in extended test
jobs or manual performance sessions.

## Developer Workflow

Before accepting a meaningful engine change, the expected workflow is:

1. run focused subsystem tests
2. run deterministic offline audio regression tests
3. run scheduler fuzz smoke
4. run key benchmark workloads
5. inspect diagnostics compatibility if protocol-facing fields changed

## Exit Criteria For Phase 9

Phase 9 is complete when the design explicitly defines:

- deterministic offline validation strategy
- audio hash regression strategy
- Black MIDI stress corpus direction
- scheduler and parser fuzzing strategy
- diagnostics regression strategy
- performance benchmark harness direction

This document is the design source of truth for those items.
