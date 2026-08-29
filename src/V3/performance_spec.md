# SVMS V3 Performance Optimization Specification

**Status:** Ready for implementation
**Target:** Intel i5-13600KF (6P+8E cores, AVX-512 NOT supported)
**Source:** Real V3 source (`V3\SVMSVoiceManager.h`, `V3\SVMSDriver.cpp`)
**Note:** Correctness investigation is CLOSED. All targets below are pure throughput optimizations.

---

## Target 1: `DynamicMPSCQueue` modulo → bitmask

**Priority:** HIGH — low-risk, easy win
**Profiler:** 4.77% self-time in `SubmitShortMsgAtQpcCancellable`

### Current behavior
- `DynamicMPSCQueue::enqueue` and `dequeue` use `position % capacity_` on every CAS retry iteration
- All existing lane capacities ARE power-of-two (131072, 131072, 65536, 32768, 32768) — confirmed from `PriorityEventIngress` constructor

### Proposed change
1. Add `static_assert` in `DynamicMPSCQueue::ConfigureCapacity`: `capacity_` must be power-of-two (or round up to next power-of-two)
2. Store `capacityMask_ = capacity_ - 1` member
3. Replace all `% capacity_` with `& capacityMask_`

### Files
- `V3\SVMSMPSCQueue.h` — `DynamicMPSCQueue` template (enqueue at ~line 440, dequeue at ~line 510)

### Correctness constraints
- Capacity MUST remain power-of-two
- The fixed `MPSCQueue<T, Capacity>` already uses `& (Capacity - 1)` — no change needed there

### Verification
- Unit tests in `tests\SVMSVoiceGrowthRegression.cpp` cover `PriorityEventIngress` which uses `DynamicMPSCQueue`
- Black MIDI stress test with 5M+ notes/sec should show reduced `SubmitShortMsgAtQpcCancellable` self-time

### Expected impact
- Eliminates integer division instruction on every CAS retry
- Modulo on power-of-two is typically 1 cycle vs 20-40 cycles for integer division
- Impact: ~0.5-1.5% total CPU reduction (from 4.77% baseline)

### SnappySynth parallel
- SnappySynth uses lock-free SPSC ring buffer (not MPSC) — no direct parallel

---

## Target 2: `VolatileHeapSiftDown` / `BuildVolatileStealHeap` layout

**Priority:** MEDIUM — 6.55% combined self-time
**Profiler:** 3.69% (SiftDown) + 2.86% (BuildVolatileStealHeap)

### Current behavior
- `BuildVolatileStealHeap` runs ONCE per output frame (gated by `stealVolatileHeapFrame_ != currentFrame_`)
- Floyd's bottom-up construction: O(V) sift-downs, each touching 3 parallel arrays (key, handle, position map)
- `VolatileHeapSiftDown`: 12+ memory accesses per swap (key swap + handle swap + 2 position-map updates)
- Position map requires random-access writes at `stealVolatileHeapPosition_[handle]` — cache-hostile for large voice counts

### Proposed change
1. **Structure-of-Arrays → Array-of-Structures for small heaps**: When `stealVolatileHeapCount_ < 64`, pack `{key, handle}` into a single 16-byte struct for better cache locality during sift. Keep position map separate (indexed by voice handle, not heap position).
2. **Batch the key computation loop**: The scalar fallback loop (lines 3074-3118) does 4 scattered memory reads per voice (`currentGain`, `stealOutputGain`, `birthFrame`, `activePosition`). Reorder to prefetch `birthFrame` and `activePosition` 2 iterations ahead (these are in the hot SoA arrays).
3. **Skip sift when key unchanged**: In `BuildVolatileStealHeap`, if `stealVolatileHeapKey_[position] == newKey` after computation, skip the sift. Many volatile voices have stable `currentGain` (sustain-tier voices that just transitioned to volatile).

### Files
- `V3\SVMSVoiceManager.h` — `VolatileHeapSiftDown:3046`, `BuildVolatileStealHeap:3062`

### Correctness constraints
- Heap invariant (min-heap on `EncodeStableWinnerKey` score) must be preserved
- Position map must stay synchronized with heap contents
- The `EncodeStableWinnerKey` order-preserving encoding is critical — must not change

### Verification
- `BuildVolatileStealHeap` already has an assertion at line 2785 verifying all volatile voices are included
- Run Black MIDI stress test and compare steal victim selection against unmodified build (deterministic given same frame)

### Expected impact
- ~2-4% total CPU reduction (from 6.55% baseline)
- Most benefit at high voice counts (20k+) where cache misses dominate

### SnappySynth parallel
- SnappySynth uses flat array with precomputed keys updated on state changes only (`ss_game_mix_float` lines 576-606). The idea of caching keys across frames is analogous but SVMS has more volatile-state transitions.

---

## Target 3: `ReuseMatchingStealGroup` data layout

**Priority:** MEDIUM — 2.51% self-time
**Profiler:** 2.51% self-time

### Current behavior
- Per stolen voice: ~50 scattered memory accesses across 5 linked lists
  - Play-group list: `v.playGroupNext`, `v.playGroupPrev`
  - Channel-note list: `v.channelNoteNext`, `v.channelNotePrev`
  - Volatile candidate list: `stealVolatileList_`, `stealVolatilePosition_`
  - Stable tree: `stealWinnerTree_`, `stealStableKey_`
  - Free stack: `freeNext_`, `freeTop_`
- Each linked-list node is a separate SoA field — 4 cache lines touched per node traversal

### Proposed change
1. **Batch the validation loop** (lines 186-238): Currently validates one voice at a time with 6-8 memory reads each. Validate in batches of 8: read `state[]`, `birthFrame[]`, `channel[]`, `note[]` arrays in SIMD-friendly layout, then AND the results to get a bitmask of valid candidates.
2. **Reduce per-voice steal-and-reinit memory accesses** (lines 340-762): The current path touches ~50 fields per voice. Consolidate the most frequently accessed fields (`state`, `currentGain`, `channel`, `note`) into a 32-byte "steal context" struct that fits in one cache line.
3. **Keep linked lists for now**: Replacing with dense arrays requires refactoring `RefreshRenderClass`, `UpdateStealCandidate`, `LaunchVoiceGroup` — too invasive for this optimization pass. The batch validation and context struct give most of the benefit.

### Files
- `V3\SVMSVoiceManager.h` — `ReuseMatchingStealGroup:3970`

### Correctness constraints
- Voice state transitions must remain atomic (no torn reads/writes during steal)
- Play-group and channel-note list integrity must be maintained
- `RefreshRenderClass` must still find correct tier classification after steal

### Verification
- Unit tests in `tests\SVMSVoiceGrowthRegression.cpp`
- Compare steal victim selection against unmodified build

### Expected impact
- ~0.5-1.0% total CPU reduction (from 2.51% baseline)
- Most benefit comes from reduced cache misses in the validation loop

### SnappySynth parallel
- SnappySynth uses flat array with linear scan — no linked lists. The "dense array" idea is the same but SVMS needs linked lists for play-group and channel-note tracking.

---

## Target 4: `PopStealCandidates` batch wiring into `LaunchVoiceGroup`

**Priority:** MEDIUM — already built, needs wiring
**Profiler:** Part of HandleNoteOn 19.20% total

### Current behavior
- `PopStealCandidates` batch wrapper exists and is unit-tested (`tests\SVMSVoiceGrowthRegression.cpp`)
- `SetStealBatchingEnabled` is already toggled in `DispatchRenderEventBatch` (line 5606-5608)
- When `stealBatchingEnabled_ == true`, `LaunchVoiceGroup` calls `PopStealCandidates` for multi-layer notes (lines 2449-2450)
- The batch is scoped per-launch (not per-run) — cross-note batching is explicitly unsupported

### Proposed change
- **Already wired.** The batch path is gated behind `!correctnessMode_` (line 5606-5608) and `count >= 2 && count <= kStealBatchMaxLayers` (line 2441-2442)
- The only remaining work is to ensure `correctnessMode_` defaults to `false` in production builds (it already does)
- No code changes needed — the batch path is active when correctness mode is off

### Files
- `V3\SVMSVoiceManager.h` — `PopStealCandidates` (batch wrapper), `LaunchVoiceGroup:4199`
- `V3\SVMSDriver.cpp` — `DispatchRenderEventBatch:5590-5608`

### Correctness constraints
- Cross-note batching is NOT supported (confirmed empirically, line 4224-4235)
- Batch must be scoped per-launch transaction only
- Rollback path (`RearmLiveBatchVictims`) must restore unconsumed victims

### Verification
- Unit tests already exist for the batch path
- `FinishLaunchTestProfile` logs batch statistics

### Expected impact
- Eliminates per-layer `PopStealCandidate` overhead for multi-layer notes (stereo SoundFonts = 2 layers)
- ~0.3-0.8% total CPU reduction

### SnappySynth parallel
- SnappySynth has no equivalent — single-layer voice allocation only

---

## Target 5: `ComputeStableStealKey` caching for stable voices

**Priority:** LOW — stable keys are constant per frame
**Profiler:** Part of `BuildStealHeap` (called once per `EnforceVoiceLimit`)

### Current behavior
- `ComputeStableStealKey` = `ageUnits - effectiveLevel * 42000 - currentFrame_/256`
- For stable voices (delay/hold/attack/sustain): `effectiveLevel = |targetGain| * stealOutputGain` — constant (targetGain doesn't change for these stages, stealOutputGain is set once at config time)
- `ageUnits = min(currentFrame_ - birthFrame, UINT32_MAX) / 256.0f` — changes once per frame
- `commonAgeScore = currentFrame_ / 256.0f` — changes once per frame
- Net: `ComputeStableStealKey = -birthFrame/256 - effectiveLevel * 42000` — CONSTANT per voice (birthFrame and effectiveLevel never change)

### Proposed change
1. Cache `ComputeStableStealKey` result in `stealStableKey_[handle]` at voice configuration time
2. In `BuildStealHeap`, read cached key instead of recomputing
3. In `UpdateStealCandidate`, only recompute if `IsStableStealCandidate` transitions (tier change)

### Files
- `V3\SVMSVoiceManager.h` — `ComputeStableStealKey:2989`, `BuildStealHeap:2667`, `UpdateStealCandidate:2920`

### Correctness constraints
- `stealStableKey_[handle]` must be updated when voice transitions between stable/volatile tiers
- `EncodeStableWinnerKey` must be applied at tree-insert time (not at cache time) because `activePosition` changes

### Verification
- Compare tree contents before/after optimization under identical workload
- The existing assertion at line 2785 verifies all volatile voices are in the heap

### Expected impact
- ~0.2-0.5% total CPU reduction
- `BuildStealHeap` is called at most once per `EnforceVoiceLimit` — not every steal

### SnappySynth parallel
- SnappySynth caches precomputed keys at state-change time (`ss_game_mix_float` lines 576-606) — same idea, different trigger

---

## Target 6: AVX-512 viability

**Priority:** N/A — NOT viable on target hardware

### Analysis
- Target CPU: Intel i5-13600KF (Raptor Lake, 14 cores: 6P+8E)
- 13th-gen Raptor Lake does NOT support AVX-512 (disabled due to P-core/E-core asymmetry)
- AVX-512 was last available on 11th-gen Rocket Lake (desktop) and Tiger Lake (mobile)
- Nova Lake (future) will bring AVX-512 back with AVX 10.2, but not relevant for current target
- Some early Alder Lake batches had AVX-512 on P-cores with E-cores disabled, but this is undocumented and unreliable

### Recommendation
- **Do not implement AVX-512 paths.** Target hardware doesn't support it.
- Existing AVX2 paths (`BuildVolatileStealKeysAVX2` at line 959) are the correct SIMD target.
- If future hardware support is desired, add runtime detection via `__cpuid` and guard with `#ifdef __AVX512F__` — but this is out of scope for current optimization.

---

## Implementation Order

1. **DynamicMPSCQueue bitmask** (Target 1) — 15 minutes, highest confidence, lowest risk
2. **PopStealCandidates wiring verification** (Target 4) — already wired, just verify correctnessMode_ default
3. **VolatileHeap layout** (Target 2) — 1-2 hours, medium complexity, highest impact
4. **ReuseMatchingStealGroup batch validation** (Target 3) — 1-2 hours, medium complexity
5. **ComputeStableStealKey caching** (Target 5) — 30 minutes, low complexity, low impact
6. **AVX-512** (Target 6) — SKIP

## Measurement Plan

1. Build with optimizations disabled (debug) → baseline profiler capture
2. Apply each optimization incrementally → profiler capture after each
3. Compare self-time deltas in VS CPU Usage profiler
4. Run Black MIDI stress test (5M+ notes/sec, 40k polyphony) for 60 seconds
5. Verify audio correctness: no pops, clicks, or wrong voices (use `SVMS_ENABLE_REFERENCE_RENDERER` comparison)

## Risk Assessment

| Target | Risk | Mitigation |
|--------|------|------------|
| MPSCQueue bitmask | Very Low | Power-of-two assertion, existing tests |
| PopStealCandidates | Very Low | Already unit-tested, just verify default |
| VolatileHeap layout | Medium | Heap invariant critical, test with reference renderer |
| ReuseMatchingStealGroup | Medium | Linked-list integrity critical, test with reference renderer |
| StableKey caching | Low | Cache invalidation on tier transition must be correct |
| AVX-512 | N/A | Skip |
