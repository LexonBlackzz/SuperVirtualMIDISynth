# Runtime Foundations

This document defines the Phase 1 runtime foundations for `VirtuallySuper`.

The goal of Phase 1 is not to fully implement the engine, but to freeze the
runtime model strongly enough that later phases can be built in manageable,
modular slices without re-litigating core ownership and memory rules.

## Scope

Phase 1 freezes:

- runtime state ownership
- zero-allocation policy
- memory pool strategy
- hot memory layout rules
- deterministic offline mode expectations
- configuration layering rules
- the modular source tree direction for implementation

## Runtime State Model

The engine is anchored around four top-level runtime states:

- `EngineState`
- `SchedulerState`
- `SceneState`
- `TelemetryState`

These states are long-lived and are created during engine initialization.

## EngineState

`EngineState` is the top-level owner for the `VirtuallySuper` runtime.

Responsibilities:

- own long-lived engine config and build-time capabilities
- own memory pools and allocator state
- own `SchedulerState`, `SceneState`, and `TelemetryState`
- own worker thread state and render context
- coordinate startup, shutdown, reset, and mode changes

Recommended composition:

- immutable or rarely changing config snapshot
- CPU/platform capability snapshot
- pool registry
- scheduler instance
- scene instance
- telemetry instance
- render orchestration state
- worker thread handles and synchronization objects

## Configuration Model

`VirtuallySuper` should be highly configurable, but runtime code should not
consume raw UI settings directly.

The intended flow is:

1. user-facing config and profiles
2. validation and normalization
3. compiled runtime config snapshot
4. hot-path consumption of compact runtime values

Recommended config layers:

- `UserConfig`
  Raw user-facing settings loaded from files or the Configurator.
- `ProfileConfig`
  Imported or built-in preset overlays such as `Reference`, `Realtime`, and
  `Extreme`.
- `ValidatedConfig`
  A normalized config object after range checking, dependency resolution, and
  fallback decisions.
- `RuntimeConfigSnapshot`
  Compact, engine-facing values consumed by realtime code.

Rules:

- UI concepts stay in the user-facing layers
- hot paths consume only `RuntimeConfigSnapshot`
- reconfiguration should swap snapshots atomically or at safe boundaries
- expert-facing controls are allowed, but should still compile down to stable
  runtime values

## Configuration Surface Areas

The engine should be configurable in at least these areas:

- output and latency
- worker and tile policy
- exact polyphony
- layer counts
- grouped-tier behavior
- density-tier behavior
- overload ladder behavior
- gain and limiting policy
- SoundFont discovery and preload policy
- diagnostics level and update cadence

The fact that many of these settings exist does not mean they all belong in the
basic Configurator surface.

## SchedulerState

`SchedulerState` owns event ingress, event timing, and same-key transition logic.

Responsibilities:

- receive normalized MIDI events
- maintain far-future scheduling state
- maintain per-key `TransitionQueue`s
- perform event reduction and admission decisions
- prepare scene updates for the current render window

Recommended composition:

- ingress rings
- future-event structure
- per-key transition tables
- same-key queue scratch
- density and burst counters
- overload-facing event pressure metrics

## SceneState

`SceneState` owns the currently audible musical scene after scheduling.

Responsibilities:

- hold exact-tier runtime objects
- hold grouped-tier runtime objects
- hold density-tier runtime objects
- hold per-channel and per-key applied state needed after scheduling
- provide job-ready views to the renderer

Recommended composition:

- channel state tables
- exact voice pool
- grouped object pool
- density object pool
- render bucket indices
- promotion/demotion bookkeeping

## TelemetryState

`TelemetryState` owns the engine-to-configurator diagnostics path.

Responsibilities:

- publish cheap snapshots
- publish coarse debug events
- expose counters without blocking the audio thread

Recommended composition:

- hot stats snapshot buffers
- warm stats snapshot buffers
- cold info cache
- debug event SPSC ring
- sequence counters

## Zero-Allocation Policy

The following threads are real-time sensitive and must not allocate after
startup:

- audio/render thread
- scheduler thread, if separate
- worker threads

Implications:

- no `new`, `delete`, `malloc`, or unbounded container growth in hot paths
- no lazy initialization in hot paths
- no string formatting in hot paths
- no UI-driven heap work in hot paths

Allowed allocation classes:

- startup allocation
- explicit reload or rebuild allocation while the engine is quiesced
- offline-only allocation paths that are never used in realtime mode

## Pool Strategy

All dynamic runtime objects should come from fixed-capacity pools.

Planned pool families:

- `ExactVoicePool`
- `GroupedObjectPool`
- `DensityObjectPool`
- `TileJobPool`
- `TransitionQueue` storage
- telemetry event ring storage

Allocation strategy:

- free-list or index-stack based object reuse
- no ownership through scattered heap pointers
- stable object handles or indices preferred over raw pointers where practical

Preferred representation:

- index-based handles for externally referenced pools
- dense or semi-dense arrays for hot iteration
- freelists for reuse

## Hot Memory Layout

Hot state should be organized to reduce cache misses and false sharing.

Rules:

- separate hot per-block state from cold metadata
- keep per-thread scratch isolated to avoid false sharing
- prefer SoA for hot render attributes that benefit SIMD or batched access
- prefer compact AoS for state that is mutated together and rarely vectorized
- align worker-owned or frequently written counters away from unrelated hot data

Suggested separation:

- hot render state
- hot scheduler state
- thread-local scratch
- cold config and metadata
- debug-only counters

## Page Locking Policy

Best-effort page locking is allowed for hot preallocated buffers if supported.

Rules:

- must be optional and non-fatal
- must degrade cleanly if the OS refuses it
- must never become a hard requirement for normal runtime startup
- should focus on the hottest buffers only, not every allocation

Likely candidates:

- render tiles
- worker scratch
- telemetry shared snapshot
- exact/grouped/density pools if the cost is acceptable

## Deterministic Offline Mode

`VirtuallySuper` must support a deterministic offline mode from the beginning.

Offline mode requirements:

- no wall-clock timing dependencies
- no nondeterministic work stealing
- stable iteration order
- stable event reduction order
- stable random seeds where randomness is used
- stable promotion and demotion decisions

Implications for design:

- worker scheduling needs a deterministic policy in offline mode
- density or phase jitter systems need explicit seeded RNG control
- telemetry and debug counters must not influence runtime decisions

## Header-Only Versus CPP Policy

Default rule:

- keep implementation in `.cpp` files
- keep headers focused on types, constants, and small inline helpers

Header-only is acceptable for:

- tiny POD utilities
- trivial constexpr helpers
- lightweight ID or handle wrappers

Avoid header-only for:

- scheduler logic
- scene reduction logic
- rendering logic
- pool implementations with nontrivial behavior
- config validation or normalization logic

## Phase 1 Exit Condition

Phase 1 is considered complete when:

- the runtime states are defined at the documentation level
- memory and allocation rules are frozen for implementation
- the initial module map is defined
- the source tree has a dedicated `src/VirtuallySuper` home
- later phases can begin without reopening foundational ownership questions
