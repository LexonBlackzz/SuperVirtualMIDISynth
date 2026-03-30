# SceneSynth Configurator Rewrite

## Phase Intent

The `VirtuallySuper` Configurator is not a small utility window.

It is the control center for:

- engine configuration
- SoundFont discovery and ordering
- live diagnostics
- performance tuning
- profile management
- compatibility visibility

This rewrite should take inspiration from the stronger parts of OmniMIDI's
Configurator and debug tooling:

- multiple clear tabs
- safe common settings on the surface
- deeper expert surfaces when requested
- a proper debug window
- obvious import/export and profile workflows

The rewrite should not inherit OmniMIDI's weaknesses:

- long, dense option walls
- too many unrelated toggles on one page
- weak grouping between "safe" and "dangerous" options
- debug visibility that is text-heavy but still coarse

## Product Rules

- The Configurator must remain observational, not intrusive.
- The synth must never block on UI work.
- If the Configurator is closed, synth cost should stay near baseline.
- If the Configurator is open, expensive graphing and formatting must happen in
  the Configurator process only.
- The Configurator must feel like one coherent product, not a pile of dialogs.

## Layout Direction

Use a tabbed shell with a small persistent header and a clear mode selector.

Persistent header content:

- engine online or offline state
- current engine name: `VirtuallySuper`
- current profile name
- selected output device summary
- current voice metrics summary
- live status pill for protocol match or mismatch

Primary navigation should be tabs, not cascading floating windows.

## Main Tabs

### Home

Purpose:

- fast overview for normal users
- quick actions
- current engine status

Content:

- profile picker
- engine online/offline state
- current SoundFont set summary
- current output summary
- exact voices / voice equivalent summary
- render time summary
- quick actions:
  - apply
  - reload config
  - hard reset
  - rescan SoundFonts
  - open debug window

### SoundFonts

Purpose:

- manage files, folders, order, and bank visibility

Content:

- explicit file list
- folder root list
- recursive scan toggle per root
- per-root enable/disable
- detected SoundFonts list
- drag reorder support
- per-font enable/disable
- per-font metadata summary
- preload or stream policy
- missing-file warnings

The list should support both:

- simple mode: "these are my folders/files"
- advanced mode: priority, preload, future bank behavior

### Engine

Purpose:

- safe runtime configuration for most users

Content:

- output device
- sample rate
- latency or buffer size
- quality profile
- worker count
- maximum exact voices
- maximum layer count
- grouped and density mode master policy

This tab is the closest equivalent to a clean "audio engine settings" page.

### Timing

Purpose:

- scheduler and articulation behavior

Content:

- accurate / quantized / hybrid timing policies
- buzz protection
- retrigger handling
- event coalescing policy
- same-key queue policy
- sustain behavior
- restart and warmup policy

This tab must explain timing tradeoffs clearly and avoid cryptic labels.

### Performance

Purpose:

- expose overload and survival controls

Content:

- overload ladder preset
- tile size
- worker model
- release shortening policy
- quiet-tail culling thresholds
- grouped admission thresholds
- density transition thresholds
- Black MIDI survival options

This is where end users can deliberately choose whether the engine values
accuracy, articulation, or survival most strongly under load.

### FX And Output

Purpose:

- final output shaping

Content:

- limiter
- final gain
- reverb
- chorus
- width
- optional future output shaping policies

### Diagnostics

Purpose:

- non-developer live visibility

Content:

- live graphs
- voice metrics
- queue metrics
- worker utilization
- overload state
- event collapse rate
- current tier mix

This is not the heavy debug window. It is the main live-monitor surface.

### Advanced

Purpose:

- expose high-complexity settings without poisoning the normal UI

Content:

- compatibility quirks
- rare scheduler policies
- aggressive grouped/density tuning
- profile internals
- explicit risk labels

### Profiles

Purpose:

- make complex engine behavior approachable

Content:

- built-in profiles
- custom profiles
- import/export
- clone
- reset to defaults
- per-profile notes

Profiles should be how most users interact with the engine.

### Developer

Purpose:

- opt-in deep control surface

Content:

- developer-only toggles
- protocol inspection
- live telemetry rate controls
- internal feature gates
- future prototype switches

This tab must be hidden by default and only appear when Developer mode is
enabled.

## Settings Surface Model

The same underlying config model should be presented through four UI layers:

- `Basic`
- `Advanced`
- `PowerUser`
- `Developer`

These are not just labels.
They define how much control the user is shown and how much warning or guidance
the UI provides.

### Basic

Audience:

- users who just want the synth to work well

Visible controls:

- output device
- sample rate
- latency
- master volume
- quality profile
- SoundFont folders/files
- maximum voices
- maximum layers
- simple timing mode

Behavior:

- strong presets
- no dangerous controls by default
- explanations written in plain language

### Advanced

Audience:

- users comfortable tuning behavior

Visible controls:

- worker count
- tile size
- preload policy
- limiter and FX options
- scheduler style
- grouped/density enable policy
- reload and discovery behavior

Behavior:

- more detail
- still guided
- inline warnings when costs are non-obvious

### PowerUser

Audience:

- users who want to shape engine internals without hacking code

Visible controls:

- exact-tier ceilings
- grouped thresholds
- density thresholds
- release shortening rules
- sustain overload rules
- culling heuristics
- timing and buzz protection tuning
- profile internals

Behavior:

- more raw values
- stronger warnings
- allow finer-grained override of presets

### Developer

Audience:

- engine developers and testers

Visible controls:

- protocol version visibility
- deep diagnostics toggles
- deterministic mode toggles
- telemetry sampling changes
- prototype feature flags
- debug counters and inspection controls

Behavior:

- hidden by default
- explicit warning banner
- no guarantee of backwards-compatible UI layout

## SoundFont UX

SoundFont management should be first-class and portable.

### Discovery Model

Support both:

- explicit file entries
- folder roots

Per-folder options:

- recursive scan on/off
- enabled on/off
- relative path preference when possible
- scan at startup on/off

### List Behavior

The UI should show:

- load order
- enabled state
- type (`sf2`, `sfz`, future packed runtime bank)
- last modified time
- preload state
- error state

The Configurator should do the expensive work:

- duplicate detection
- metadata formatting
- conflict warnings
- sorting and filtering

The synth should only provide:

- currently loaded banks
- IDs
- coarse memory counters
- change notifications

### Portability Rules

- store relative paths whenever practical
- avoid machine-specific absolute-path dependence when a portable path exists
- support keeping SoundFonts near the DLL/config folder
- do not require background services or shell integration

## Diagnostics Views

There should be two different surfaces:

- `Diagnostics` tab in the main Configurator
- dedicated `Debug Window`

### Diagnostics Tab

Goal:

- high-level live monitoring for normal users

Views:

- render ms graph
- queue depth graph
- exact voices
- layer instances
- grouped objects
- voice equivalent
- overload state
- event collapse rate

### Debug Window

Goal:

- deeper investigation without bloating the main UI

Suggested tabs:

- `Summary`
- `Voices`
- `Scheduler`
- `Render`
- `SoundFonts`
- `System`
- `Protocol`
- `Log`

#### Summary

- engine status
- current profile
- current voice metrics
- current render metrics
- protocol status

#### Voices

- exact voices
- layer instances
- grouped objects
- density objects
- steals
- culls
- per-channel activity

#### Scheduler

- ingress pressure
- future queue depth
- same-key queue depth
- late events
- collapse counts
- sustain pressure

#### Render

- worker activity
- tile counts
- helper path hit counts
- tier mix
- merge cost

#### SoundFonts

- loaded banks
- memory estimates
- preload status
- discovery results

#### System

- device info
- latency summary
- process-level memory snapshots
- thread counts

#### Protocol

- bridge version
- protocol compatibility status
- snapshot cadence
- debug event ring health

#### Log

- coarse structured events only
- overload entered/exited
- reloads
- missing fonts
- protocol mismatch notices

## Graph Set

Recommended baseline graph set:

- render ms
- audio latency estimate
- exact voices
- layer instances
- grouped objects
- voice equivalent
- queue depth
- late events
- steals/culls
- event collapse rate
- tier mix over time

Rules:

- graph histories live only in the Configurator process
- synth side publishes counters, not chart data
- graphs may be disabled independently of the rest of diagnostics

## Profile System

Profiles are essential because the engine will be highly configurable.

### Built-In Profiles

Suggested initial built-ins:

- `Safe`
- `Reference`
- `Realtime`
- `Black MIDI`
- `Extreme`

### Custom Profiles

Users should be able to:

- create
- clone
- rename
- export
- import
- reset

### Profile Format

Preferred direction:

- human-readable file format
- versioned schema
- portable path support
- explicit engine target name

Recommended format:

- `yaml` or `jsonc`

The file should store:

- display name
- description
- settings visibility level
- normalized config values
- optional SoundFont set references
- schema version

## Versioning And Protocol Mismatch Strategy

The Configurator must be strict and explicit about protocol compatibility.

Rules:

- exact protocol version match is preferred
- layout or schema mismatch must be surfaced clearly
- the Configurator must never silently misinterpret stats

UI behavior:

- green status when compatible
- yellow warning for older-but-readable metadata-only cases
- red mismatch state when live control or stats are unsafe

Mismatch actions:

- show detected DLL/bridge version
- show Configurator expected version
- explain whether monitoring-only fallback is possible
- disable unsafe controls automatically

## Accessibility And Usability Rules

- avoid giant ungrouped walls of controls
- use clear descriptions and warnings
- keep dangerous settings visually distinct
- support keyboard navigation
- support DPI scaling well
- prefer grouped cards/panels over long legacy forms

## Default Presentation Strategy

Recommended default startup behavior:

- open in `Basic`
- land on `Home`
- show profile picker prominently
- keep `Developer` hidden
- keep the main window useful even if no live engine is attached

## Phase 8 Exit Condition

Phase 8 is complete when the design explicitly defines:

- final tab structure
- settings surface model
- SoundFont UX
- diagnostics and debug views
- graph set
- profile system
- protocol mismatch UX

This document is the design source of truth for those items.
