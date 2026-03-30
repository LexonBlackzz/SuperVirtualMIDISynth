# Tile Renderer And Threading

This document defines the Phase 6 tile renderer and threading design for
`VirtuallySuper`.

This phase is performance-critical. The render architecture must preserve cache
locality, deterministic behavior, and a clear path for AVX/SSE optimization.
Threading is not treated as a generic work queue system; it is treated as a
carefully shaped extension of the render pipeline.

## Goals

- keep render work cache-friendly
- avoid giant whole-block worker buffers as the primary model
- support SIMD-friendly batching for exact, grouped, and density work
- minimize merge bandwidth cost
- preserve deterministic offline behavior
- keep scalar fallbacks available for divergent cases without polluting hot
  vector paths

## Tile Size Policy

The default render policy should be fixed-size tiles.

Recommended default tile sizes:

- `128` samples as the latency-friendly baseline
- `256` samples as the throughput-oriented large tile

Policy:

- prefer `128` as the standard realtime tile
- allow `256` when workload or backend favors throughput
- avoid large monolithic block rendering as the normal threaded unit

Reasons:

- keeps hot working sets smaller
- improves L1/L2 locality
- reduces giant end-of-block merge traffic
- makes scheduler and scene updates easier to reason about in bounded windows

## Tile Ownership

Each tile should be an explicit render unit with:

- tile start sample
- tile frame count
- render bucket references
- exact/grouped/density job slices
- output target description

Tiles should be described compactly and handed to workers through fixed-capacity
job structures.

## Job Queue Format

The job queue should be fixed-capacity and realtime-safe.

Suggested `TileJob` contents:

- `tileId`
- `tileStartSample`
- `tileFrames`
- `jobKind`
- `bucketRangeBegin`
- `bucketRangeEnd`
- `workerHint`
- `outputSliceId`
- `flags`

Suggested `jobKind` values:

- `ExactRender`
- `GroupedRender`
- `DensityRender`
- `TileReduce`
- `TileFinalize`

The exact structure may evolve, but jobs must remain:

- POD-like
- fixed-size
- cheap to enqueue and dequeue
- independent of heap ownership

## Worker Thread Model

The worker model should be tile-oriented and bucket-aware.

Rules:

- workers process tile jobs, not whole-audio-block voice partitions
- the main thread may participate in rendering
- workers should be given jobs that preserve sample locality where possible
- thread wakeups should be bounded and avoid pathological micro-fragmentation

Preferred behavior:

- exact, grouped, and density jobs can be interleaved per tile
- worker assignment may use hints, but correctness must not depend on a
  specific worker getting a specific job
- small tiles should still be renderable single-threaded when threading overhead
  would dominate

## Threading Threshold Policy

Threading should activate only when it is likely to win.

A workload gate should consider:

- tile count
- total job count
- estimated exact/grouped/density work
- current worker availability
- current backend latency mode

Design rule:

- tiny fragments should stay single-threaded
- large sustained render windows should exploit workers

## Merge Path

The merge path must be treated as a bandwidth bottleneck risk.

Rules:

- prefer tile-local reduction over giant whole-block reduction
- keep merge windows small
- avoid reading and writing megabytes of intermediate buffers when a tile-sized
  reduction would suffice
- use vector add paths for reduction where profitable

Preferred model:

- workers render into tile-scoped intermediate slices
- tile reduction happens while the tile is still hot
- final output is produced incrementally by tile

This should be the default strategy instead of:

- one full buffer per worker
- one giant merge at the end

## SIMD Strategy

SIMD is a first-class design concern, not a late micro-optimization.

The renderer should explicitly support:

- scalar fallback path
- SSE2-compatible vector path
- AVX2 path
- optional future AVX-512 path if ever justified

The design must not sabotage vectorization with unnecessary abstraction in hot
loops.

## SIMD Helper Boundaries

The render design should maintain explicit helper classes or functions for:

- exact contiguous render
- exact gather-style render
- grouped render kernels
- density grain kernels
- tile reduction kernels

Rules:

- branch once per helper selection where possible
- keep the inner loop specialized
- avoid mixing wildly divergent cases in one monolithic render body

## AVX And SSE Expectations

Hot vector paths should be designed around:

- lane-friendly grouping
- page-local sample access
- minimized gather/scatter pain
- predictable tails and cleanup

AVX/SSE policy:

- AVX2 should be the main wide path on supported x64/x86 CPUs
- SSE2 should remain the compatibility vector floor
- scalar must remain correct and available for divergent or tiny cases

## Scalar Fallback Policy

Scalar is not just a legacy fallback. It is required for:

- tiny fragments
- highly divergent modulation
- cases where vector setup cost outweighs benefit
- complex exact paths that do not batch cleanly

The design should keep scalar paths narrow and explicit rather than letting
scalar behavior leak through every vector helper.

## Exact Tier Render Strategy

Exact rendering should prefer:

- contiguous sample access when possible
- gather or mixed-index paths only when needed
- specialized helper dispatch instead of one mega-loop
- page-local or pitch-local bucket ordering to improve locality

Exact render buckets should be shaped to keep SIMD utilization high without
corrupting musical correctness.

## Grouped Tier Render Strategy

Grouped rendering should be even more SIMD-friendly than exact rendering.

Reasons:

- grouped objects already trade some individuality for shared work
- grouped buckets should be designed to improve lane coherence
- page and pitch locality should be stronger than in the exact tier

## Density Tier Render Strategy

Density rendering should favor:

- stable grain kernels
- vector-friendly cloud generation
- compact state per density object
- cheap reduction into the tile output

Density should never become a hidden scalar disaster just because it is a
fallback tier.

## Deterministic Offline Worker Policy

Offline mode must remain deterministic.

Required behavior:

- stable job ordering
- stable tile ordering
- stable reduction ordering
- no nondeterministic work-stealing effects
- seeded deterministic randomness for density or grain jitter

Allowed approach:

- offline mode may use a stricter worker schedule than realtime mode
- deterministic offline execution is more important than maximum offline speed

## Thread-Local Scratch

Each worker should have thread-local scratch for:

- temporary accumulators
- SIMD tails
- tile-local metadata
- helper-local temporary arrays where unavoidable

Rules:

- no sharing of worker scratch across threads
- no hot false sharing
- size thread-local scratch for the chosen tile policy

## Performance Invariants

The render design should preserve these invariants:

- no hot-path allocation after startup
- no whole-engine lock contention in the render loop
- no UI dependency in the render loop
- no giant merge bottleneck as the default path
- no architecture that makes AVX/SSE optimization awkward by construction

## Phase 6 Exit Condition

Phase 6 is considered complete when:

- tile size policy is specified
- job queue format is specified
- worker thread model is specified
- tile merge path is specified
- deterministic offline worker behavior is specified
- scalar and SIMD helper strategy is specified
