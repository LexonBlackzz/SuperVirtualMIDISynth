# SceneSynth Configurator And Diagnostics

## Design Intent

The Configurator must become a proper control center for the engine without
meaningfully slowing the synth itself down.

The synth publishes cheap raw facts.
The Configurator performs expensive interpretation.

## Hard Rule

- If the Configurator is closed, synth overhead should remain near baseline.
- If the Configurator is open, synth overhead should rise only slightly.
- If the Configurator stalls or crashes, audio must continue unaffected.

## Configuration Philosophy

`VirtuallySuper` should support deep configurability without forcing complexity
onto every user.

The intended model is:

- `Basic`
  Safe day-to-day settings for most users.
- `Advanced`
  More detailed tuning for power users.
- `PowerUser`
  Voice, layer, grouping, density, and overload controls.
- `Developer`
  Experimental or diagnostic-only settings.

Important rule:

- the synth consumes normalized config snapshots
- the Configurator owns presentation, grouping, tooltips, warnings, and
  discoverability

## Safe Versus Expert Settings

Examples of `Basic` settings:

- audio output device
- sample rate
- buffer or latency
- master polyphony target
- layer count
- SoundFont folders and ordering
- quality profile

Examples of `Advanced` settings:

- worker count
- tile size
- interpolation quality
- preload policy
- limiter and FX policy
- general scheduler mode

Examples of `PowerUser` settings:

- exact-tier ceilings
- grouped-tier admission thresholds
- density-tier enable or disable policy
- release shortening thresholds
- quiet-tail culling policy
- sustain overload behavior
- per-tier gain scaling and collapse heuristics
- per-module experimental tuning values

Examples of `Developer` settings:

- debug counters
- deterministic stress switches
- instrumentation cadence
- sampled voice inspection toggles

## Diagnostics Transport

## Shared Snapshot

Use a fixed shared-memory block with:

- sequence or generation counter
- double-buffered or seqlock-style snapshot access
- no text formatting in the DLL

Synth side writes:

- counters
- gauges
- IDs
- fixed-capacity arrays for small summaries

Configurator side computes:

- rolling averages
- sparkline or graph histories
- rankings
- text summaries
- thresholds and visual warnings

## Event Ring

Use a fixed SPSC lock-free ring for coarse debug events:

- overload entered/exited
- bank reload
- worker stall
- dropped event burst
- font discovery changes
- reset/restart events

Do not use the event ring for per-note spam.

## Update Tiers

### Hot Stats

Updated very often:

- render ms
- audio latency estimate
- exact voices
- voice equivalent
- queue depth
- overload state
- worker/tile counts

### Warm Stats

Updated less often:

- per-channel counts
- grouped vs exact ratios
- cull and steal counts
- SoundFont usage summaries
- scheduler queue ages

### Cold Info

Updated on change only:

- loaded SoundFonts
- discovered folders
- engine mode
- device info
- profile data

## Proposed Configurator Tabs

### Home

- engine online/offline state
- selected profile
- current backend or engine mode
- loaded SoundFonts summary
- quick actions

### SoundFonts

- explicit font list
- folder watch list
- recursive scan toggle
- enable/disable
- order and priority
- preload policy
- metadata cache

### Engine

- sample rate
- latency
- tile size
- worker count
- max exact voices
- max layers
- quality profile

This tab should default to safe settings, with advanced sections hidden behind
an expand/collapsible expert surface.

### Timing

- accurate or quantized behavior
- buzz protection
- event reduction policy
- sustain pressure behavior
- per-note queue behavior

### Performance

- overload ladder profile
- culling aggressiveness
- layer reduction thresholds
- density thresholds
- CPU safety settings

This is the most likely home for end-user-facing expert controls over voices,
layers, grouped behavior, and density policy.

### FX And Output

- output device
- limiter
- reverb and chorus
- width and spatial controls
- final gain policy

### Diagnostics

- live graphs
- counters
- queue health
- worker utilization
- tier distribution

### Advanced

- experimental toggles
- compatibility settings
- debug controls

This tab should be clearly separated from normal user settings so the engine can
remain extremely configurable without becoming hostile to ordinary use.

### Profiles

- import and export
- built-in presets such as `Reference`, `Realtime`, and `Extreme`

Profiles should be the primary way most users interact with the full engine
flexibility without hand-editing dozens of expert settings.

## SoundFont Discovery

Portable folder support should be first-class:

- relative paths stored when possible
- explicit folder roots
- scan on startup and on command
- no external service required

Suggested behavior:

- allow one or more root folders
- allow recursion toggle per root
- cache font metadata
- support reorder and per-font enable/disable

## Debug Window

A dedicated debug view should replace the current coarse panel.

Suggested debug tabs:

- `Summary`
- `Voices`
- `Scheduler`
- `Render`
- `SoundFonts`
- `System`
- `Log`

Suggested graphs:

- render ms
- queue depth
- exact voices
- voice equivalent
- dropped events
- steals/culls
- tier mix over time

## Metrics To Surface

- exact voices
- layer instances
- grouped objects
- voice equivalent
- scheduler pressure
- sustain pressure
- event collapse rate
- promoted exact notes
- density load
- worker utilization

## Developer Modes

### Normal

- cheap counters only

### Debug

- more detailed counters
- sampled top-N voices or groups

### Deep Debug

- opt-in developer mode only
- heavier snapshots at reduced rate

The synth should never silently enter a heavy diagnostics mode.
