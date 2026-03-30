# Diagnostics And Live Protocol

This document defines the Phase 7 diagnostics and live protocol design for
`VirtuallySuper`.

The central rule is unchanged:

- the synth publishes cheap raw facts
- the Configurator performs the expensive interpretation

This document formalizes how that should work.

## Goals

- expose useful live state without slowing the synth down
- keep transport lock-free or near-lock-free for the hot path
- separate frequently changing stats from rarely changing metadata
- support `Basic`, `Advanced`, `PowerUser`, and `Developer` user surfaces
- keep protocol evolution manageable and versioned

## Diagnostics Layers

The diagnostics system should support four user-facing layers:

- `Basic`
- `Advanced`
- `PowerUser`
- `Developer`

These layers affect presentation and visibility, not the hot-path ownership of
the data.

### Basic

Intended for ordinary users.

Shows:

- engine online state
- output and latency basics
- exact voices
- voice equivalent
- loaded SoundFonts summary
- coarse overload state

### Advanced

Intended for users tuning performance and quality.

Shows:

- worker and tile activity
- queue depth
- grouped versus exact ratios
- limiter and render timing summaries
- SoundFont preload and usage summaries

### PowerUser

Intended for users tuning voices, layers, density, and overload behavior.

Shows:

- grouped and density object counts
- release shortening and cull counters
- event collapse rates
- tier handoff activity
- sustain pressure and scheduler pressure details

### Developer

Intended for debugging and implementation validation.

Shows:

- deeper counters
- sampled object snapshots
- protocol and version diagnostics
- deterministic-mode visibility
- debug event stream

## Transport Components

The diagnostics transport should have two primary pieces:

1. `Shared Snapshot`
2. `Debug Event Ring`

Optional future piece:

3. `On-Demand Inspection Channel`

The on-demand channel should remain optional and never be required for ordinary
live operation.

## Shared Snapshot

The shared snapshot is the main live telemetry path.

Requirements:

- fixed shared-memory layout
- versioned
- cheap for the synth to write
- cheap for the Configurator to read
- safe if the reader misses updates

Recommended write model:

- double-buffered snapshot or seqlock-style snapshot
- synth writes inactive buffer
- synth publishes generation/sequence
- UI reads latest complete buffer

## Snapshot Groups

The snapshot should be split conceptually into:

- `HotStats`
- `WarmStats`
- `ColdInfo`

### HotStats

Updated most frequently.

Suggested contents:

- render time
- estimated output latency
- exact voice count
- voice equivalent
- grouped object count
- density object count
- overload state
- worker activity
- tile count

### WarmStats

Updated less frequently.

Suggested contents:

- per-channel activity summaries
- scheduler queue depths
- event collapse rates
- steals and culls
- grouped-to-density handoffs
- release shortening metrics
- SoundFont usage summaries

### ColdInfo

Updated only on change or reconnect.

Suggested contents:

- engine version
- protocol version
- config profile name
- loaded SoundFonts and folder roots
- selected output device
- active engine mode
- platform capability summary

## Shared Snapshot Schema Rules

Rules:

- no strings that require heap ownership in hot snapshots
- prefer fixed-size arrays and IDs
- use counters, gauges, and compact metadata
- keep sizes bounded and documented
- separate hot and cold fields when possible

The Configurator should resolve:

- names
- formatting
- long text
- historical graphs

## Debug Event Ring

The debug event ring is for coarse event notifications, not per-note traffic.

Requirements:

- fixed-capacity SPSC ring
- synth pushes
- Configurator pulls
- overwrite or drop policy must be explicit

Good event types:

- overload entered or exited
- profile changed
- SoundFont reload
- worker stall detected
- reset or restart
- dropped-event burst
- deterministic mode entered

Bad event types:

- every note-on
- every voice start
- per-grain density spam

## Event Ring Schema

Suggested event fields:

- `eventKind`
- `timestampSample`
- `severity`
- `arg0`
- `arg1`
- `arg2`

The event ring should stay numeric and compact.

## Update Cadence

Cadence should be tiered rather than uniform.

Suggested policy:

- `HotStats`: every block or every few blocks
- `WarmStats`: lower rate, such as 10 to 20 Hz equivalent
- `ColdInfo`: on change only
- `Debug Event Ring`: on event occurrence

The exact cadence may differ between realtime and offline modes, but the synth
must remain in control of its own overhead.

## Debug Levels

The diagnostics system should support explicit debug levels.

Suggested levels:

- `Off`
- `Basic`
- `Advanced`
- `PowerUser`
- `Developer`

Rules:

- higher levels may enable more counters
- higher levels must never force massive hot-path allocations
- developer-only deep inspection should remain opt-in
- ordinary builds should still expose enough data for a good Configurator

## Protocol Versioning

The live protocol must be versioned explicitly.

Rules:

- snapshot layouts are versioned
- event ring layouts are versioned
- incompatible changes must bump the protocol version
- the Configurator should detect mismatch cleanly

## Future On-Demand Inspection

If later needed, a separate optional inspection channel may be added for:

- sampled top-N exact voices
- sampled grouped objects
- sampled density objects

Rules:

- never required for normal telemetry
- never allowed to interfere with the render thread
- may update at much lower rates than the shared snapshot

## Performance Invariants

The diagnostics system must preserve these invariants:

- no blocking the render thread
- no per-note UI formatting in the synth
- no protocol path that forces the Configurator to be present
- no assumption that the reader is always caught up

## Phase 7 Exit Condition

Phase 7 is considered complete when:

- hot, warm, and cold stat groups are specified
- shared snapshot format is specified at the design level
- debug event ring is specified
- update cadence is specified
- debug level policy is specified
- user-surface layering is aligned with diagnostics visibility
