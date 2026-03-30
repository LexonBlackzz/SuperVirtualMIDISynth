# Module Map

This document defines the initial modular source layout for `VirtuallySuper`.

The goal is maintainability first, without giving up the ability to keep hot
code physically close when measurement proves it matters.

## Source Root

The runtime engine should live under:

- `src/VirtuallySuper/`

This keeps the new engine physically separate from the existing TSF, SFZ, and
BASSMIDI-oriented code while it is being developed.

## Foundation Modules

- `VirtuallySuperTypes.h`
  Shared POD-style types, enums, IDs, constants, and lightweight handles.
- `VirtuallySuperEngine.h/.cpp`
  Top-level engine lifetime, integration surface, and ownership root.
- `VirtuallySuperConfig.h/.cpp`
  Runtime config ingestion, validation, translation, and snapshot compilation.
- `VirtuallySuperPools.h/.cpp`
  Fixed-capacity pools, freelists, and allocation helpers.

Possible later split if config complexity justifies it:

- `VirtuallySuperConfigProfiles.h/.cpp`
  Built-in and imported profile handling.
- `VirtuallySuperConfigValidate.h/.cpp`
  Validation and normalization rules.

## Scheduler Modules

- `VirtuallySuperScheduler.h/.cpp`
  Main scheduler orchestration.
- `VirtuallySuperSchedulerQueues.h/.cpp`
  Ingress rings, future-event structures, and transition queue mechanics.
- `VirtuallySuperSchedulerReduce.h/.cpp`
  Event collapse, admission, and macro-culling logic.

## Scene Modules

- `VirtuallySuperScene.h/.cpp`
  Scene ownership and orchestration.
- `VirtuallySuperSceneReduce.h/.cpp`
  Exact/grouped/density scene update logic.
- `VirtuallySuperSceneScore.h/.cpp`
  Importance scoring and promotion/demotion heuristics.

## Exact-Tier Modules

- `VirtuallySuperExact.h/.cpp`
  Exact-tier orchestration.
- `VirtuallySuperExactVoices.h/.cpp`
  Exact voice runtime behavior.
- `VirtuallySuperExactSteal.h/.cpp`
  Steal policy, release policy, and quiet-tail logic.

## Grouped-Tier Modules

- `VirtuallySuperGrouped.h/.cpp`
  Grouped object orchestration.
- `VirtuallySuperGroupedBuckets.h/.cpp`
  Bucketing and grouping keys such as sample page, pitch band, and timing.

## Density-Tier Modules

- `VirtuallySuperDensity.h/.cpp`
  Density-tier orchestration.
- `VirtuallySuperDensityGranular.h/.cpp`
  First candidate density implementation based on granular clouds.

## Render Modules

- `VirtuallySuperRender.h/.cpp`
  Top-level tile render orchestration.
- `VirtuallySuperRenderJobs.h/.cpp`
  Tile jobs and job queue helpers.
- `VirtuallySuperRenderWorkers.h/.cpp`
  Worker management and deterministic offline worker policy.

## Telemetry Modules

- `VirtuallySuperTelemetryShared.h`
  Shared-memory visible structs and versioned wire-facing definitions.
- `VirtuallySuperTelemetry.h/.cpp`
  Snapshot writing and debug event publishing.

## Overload Modules

- `VirtuallySuperOverload.h/.cpp`
  Overload ladder logic and adaptation policy.

## Rules For Splitting Files

- each subsystem should own its own state and policy files
- avoid giant catch-all utility files
- avoid pushing major logic into headers without measurement
- keep cold config and telemetry code separate from hot render logic
- keep scheduler and scene logic separate, even if they interact closely
- keep user-facing configuration handling separate from runtime hot-path code

## When To Merge Files

Merging is acceptable only when:

- profiler evidence shows a real benefit
- the code is tightly coupled enough that the split actively harms clarity
- the merged file still remains maintainable

Default assumption:

- start split
- merge only when measured need is clear

## Phase 1 Scaffold

The initial scaffold does not need full implementations.

It should provide:

- the `src/VirtuallySuper/` folder
- a README for the runtime engine folder
- a place for later modular files to land without crowding the existing engine code
