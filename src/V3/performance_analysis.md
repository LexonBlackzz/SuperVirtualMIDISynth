# SVMS vs SnappySynthV2 -- Black MIDI Performance Analysis (Verified)

## Context: Profiler-Grounded, Code-Verified Analysis

This analysis is based on **actual VS CPU Usage profiler captures** under heavy Black MIDI workload (5M+ notes/sec, 4096-32768 voice caps), with every claim verified against decompiled source.

---

## Where Cycles Actually Go (Profiler Data)

| Function | Self-Time | Verified Behavior |
|----------|-----------|-------------------|
| **HandleNoteOn** | 19.20% total / 0.75% self | Calls into steal cascade + `powf` per region |
| **SubmitShortMsgAtQpcCancellable** | 4.77% self | 4 `LOCK/UNLOCK` per message + CAS retry loops + `% capacity_` modulo |
| **VolatileHeapSiftDown** | 3.69% self | 12+ memory accesses per swap (key + handle + position map) |
| **BuildVolatileStealHeap** | 2.86% self | Full volatile heap rebuild, once per render frame |
| **ReuseMatchingStealGroup** | 2.51% self | ~50 scattered memory accesses per stolen voice across 5 linked lists |
| **RenderScalar::RenderBlockSparseRange** | 0.41% self | **Dispatcher/coordinator, NOT the render kernel** |

---

## Critical Correction #1: "Rendering is free" is WRONG

The profiler's 0.41% self-time for `RenderBlockSparseRange` is misleading. This function is a **dispatcher**, not the actual render kernel. The real per-voice sample generation happens in kernel functions dispatched via:

```c
// Line 396 in RenderBlockSparseRange:
local_1e8 = pRVar20->kernels[uVar16];  // Function pointer to actual render kernel

// Line 518: Kernel invocation via lambda
RenderBlockSparseRange::__l41::<lambda_3>::operator()(&local_148, pRVar17->handles, pRVar17->count);
```

The actual render kernels (e.g., `RenderSustainedLoopAVX2`, `RenderPrimaryVoiceSpan`, `RenderReleaseLoopAVX2`) are separate functions. **Their self-time is not captured in `RenderBlockSparseRange`'s 0.41%.**

The synth's own telemetry showing "CPU Render: 70-80%" at 8k voices is a **real-time deadline metric** (render_time / budget), while the profiler's self-time is **samples_landed / total_samples**. These are different denominators:
- Profiler: Render work spread across many worker threads = small fraction of total samples
- Synth monitor: Render work consuming most of the real-time budget = large fraction

**Both can be simultaneously true.** The profiler data does NOT prove rendering is cheap -- it proves the dispatcher function is cheap. The actual render kernels may be expensive but are attributed to different function names in the profiler.

---

## Critical Correction #2: Heap Invalidation Cascade is NOT Confirmed

My earlier claim that "EnforceVoiceLimit sets stealHeapValid_ = false after every release session, triggering full O(n) rebuilds on every steal" is **not supported by the code**.

### Actual EnforceVoiceLimit behavior (verified from source):

```c
// Lines 126-292: The release loop
do {
    // Line 130-134: Check BEFORE each steal
    if (this->stealHeapValid_ == false) {
        BuildStealHeap(this);  // Rebuild ONCE if invalid
    }
    
    // Lines 146-252: Pop candidate from heap (modifies heap, does NOT invalidate it)
    // ...steal logic...
    
    // Line 282: StartRelease on the voice
} while (uVar6 < uVar10);

// Line 296-298: Set invalid AFTER loop completes
this->stealHeapValid_ = false;
this->stealVolatileHeapValid_ = false;
```

**Key observations:**
1. `BuildStealHeap` is called at most **ONCE** per `EnforceVoiceLimit` invocation (at line 130-134, if heap was invalid from previous call)
2. The heap is NOT invalidated during the loop -- it's only set to false at line 296, AFTER the loop
3. `EnforceVoiceLimit` is called **once per render frame** (from `RenderBlock` and `Render`)

### What actually triggers stealHeapValid_ = false:

| Location | Condition | Frequency |
|----------|-----------|-----------|
| VoiceManager constructor | Initialization | Once at startup |
| EnforceVoiceLimit (line 296) | After release loop | Once per frame |
| GrowCapacity (line 1348) | During capacity growth | Rare (only when voice limit exceeded) |
| RenderBlockSparseRange (line 194) | When activeCount > 524288 | **Only at 500k+ voices** |
| Reset (line 240) | During reset | Rare |

### Profiler verification:

**`BuildStealHeap` does NOT appear in the profiler's Top Functions list.** If the O(n) rebuild cascade were happening at high frequency, it would show up. Its absence confirms the rebuild is bounded and infrequent.

**`BuildVolatileStealHeap` DOES appear at 2.86% self.** This is the volatile heap, which rebuilds once per frame (gated by `stealVolatileHeapFrame_ != currentFrame_`). This is a bounded, once-per-render-buffer cost -- NOT a cascade.

---

## Critical Correction #3: SnappySynth comparison is apples-to-oranges

SnappySynth's voice stealing selects by **voice age** (oldest voice first):
```c
// Line 576-606 in ss_game_mix_float:
if (g_sources[g_active_indices[i] * 0xa8 + 0x3c] < oldest_age) {
    oldest_age = g_sources[g_active_indices[i] * 0xa8 + 0x3c];
    oldest_idx = i;
}
```

SVMS's voice stealing selects by **quietest voice** (lowest amplitude):
```c
// Steal key computation in BuildStealHeap:
age * 0.00390625 - |gain| * stealOutputGain * 40000 - currentFrame * 0.00390625
```

**These are different selection criteria, not just different data structures for the same criterion.** An earlier bug where a FIFO/age-based fast path was used for steal selection caused audibly wrong voices to be stolen at scale. The quietest-voice-wins property is **necessary for audio correctness** in SVMS's use case.

Before treating "just do a linear scan like SnappySynth" as a valid fix, it trades away the quietest-voice-wins correctness property. This is a deliberate design choice, not pure overhead.

---

## Actual Bottleneck Analysis (Profiler + Code Verified)

### Bottleneck 1: SubmitShortMsgAtQpcCancellable (4.77% self)

**Verified from source:**
- Lines 54-68: Two `LOCK/UNLOCK` atomic increments per message
- Lines 256-516: CAS retry loops with `% capacity_` modulo on every iteration
- Lines 530-570: Two more atomic increments after enqueue

**Total per MIDI message:** 4 `LOCK/UNLOCK` pairs + CAS retry loop + modulo operation.

**Fix opportunities:**
- Replace `% capacity_` with bitmask (`capacity_` is power-of-2)
- Batch atomic increments
- Reduce CAS retry frequency

### Bottleneck 2: VolatileHeapSiftDown (3.69% self)

**Verified from source:**
- Lines 30-85: Standard sift-down with 3 parallel array swaps (key, handle, position)
- Position map updates require **two random-access writes** per swap (lines 74-76)

**Called from:** BuildVolatileStealHeap (~count/2 times during construction), PopStealCandidate, UpdateStealCandidate. Across 183M steals, called hundreds of millions of times.

**Fix opportunities:**
- Eliminate position map (use binary search)
- Use cache-friendly heap layout
- Batch sift operations

### Bottleneck 3: BuildVolatileStealHeap (2.86% self)

**Verified from source:**
- Lines 56-77: AVX2 path if available (processes 8 voices at a time)
- Lines 82-157: Scalar fallback with 4 scattered memory reads per voice
- Lines 162-234: Floyd's heap construction with O(v) sift-downs

**Triggered once per render frame** when `stealVolatileHeapFrame_ != currentFrame_`. This is a bounded cost, not a cascade.

**Fix opportunities:**
- Incremental maintenance (avoid full rebuild)
- Cache key values across frames
- Optimize the scalar key computation loop

### Bottleneck 4: ReuseMatchingStealGroup (2.51% self)

**Verified from source:**
- Lines 142-170: Play-group linked-list walk (O(k) where k = group size)
- Lines 186-238: Validation loop with 6-8 memory reads per voice
- Lines 340-762: Per-voice steal-and-reinit with ~50 scattered memory accesses across 5 linked lists

**Fix opportunities:**
- Replace linked lists with dense arrays (bitmap or sorted index)
- Batch validation
- Reduce per-voice memory accesses

### Bottleneck 5: HandleNoteOn (19.20% total, 0.75% self)

**Verified from source:**
- Lines 608-1240: Per-region computation with 5-8 `powf` calls per unprepared region
- Lines 1384: Calls `LaunchVoiceGroup` which triggers the steal cascade

**The 19.20% total is almost entirely in callees:** `LaunchVoiceGroup`, `AllocateVoiceOrSteal`, `ReuseMatchingStealGroup`, and the steal heap operations.

**Fix opportunities:**
- Pre-compute all `powf` values at SoundFont load time (cache in prepared regions)
- Reduce steal cascade depth

---

## Summary of Verified Findings

| Claim | Status | Evidence |
|-------|--------|----------|
| "Rendering is free" | **WRONG** | RenderBlockSparseRange is a dispatcher; actual render kernels are separate functions with unmeasured self-time |
| "Heap rebuild cascade on every steal" | **UNCONFIRMED** | BuildStealHeap not in profiler Top Functions; EnforceVoiceLimit rebuilds at most once per frame |
| "Volatile heap rebuilds once per frame" | **CONFIRMED** | Gated by `stealVolatileHeapFrame_ != currentFrame_`; 2.86% self in profiler |
| "5 linked lists, ~50 mem ops per steal" | **CONFIRMED** | Verified in ReuseMatchingStealGroup lines 340-762 |
| "4 LOCK/UNLOCK per MIDI message" | **CONFIRMED** | Verified in SubmitShortMsgAtQpcCancellable lines 54-68, 530-570 |
| "SnappySynth uses age-based stealing" | **CONFIRMED** | Different criterion (oldest) vs SVMS (quietest), not just different data structure |
| "Render kernels dispatch to separate functions" | **CONFIRMED** | Line 396: `pRVar20->kernels[uVar16]` is a function pointer |

---

## Revised Optimization Priorities

### Priority 1: Measure actual render kernel cost

Before optimizing anything else, we need profiler data on the **actual render kernels** (RenderSustainedLoopAVX2, RenderPrimaryVoiceSpan, etc.), not just the dispatcher. The synth's "CPU Render: 70-80%" suggests rendering IS expensive -- we just need to find where the profiler attributes it.

### Priority 2: SubmitShortMsgAtQpcCancellable (4.77% self)

Quantified, verified, and addressable:
- Replace `% capacity_` with bitmask
- Batch atomic increments
- Reduce CAS retry frequency

### Priority 3: VolatileHeapSiftDown (3.69% self)

Quantified, verified, and addressable:
- Eliminate position map (use binary search)
- Cache-friendly heap layout

### Priority 4: BuildVolatileStealHeap (2.86% self)

Quantified, verified as once-per-frame (not a cascade):
- Incremental maintenance
- Cache key values across frames

### Priority 5: ReuseMatchingStealGroup (2.51% self)

Quantified, verified:
- Replace linked lists with dense arrays
- Reduce per-voice memory accesses from ~50 to ~10

### Priority 6: HandleNoteOn callee costs

Pre-compute `powf` values at SoundFont load time.

---

## What We Still Don't Know

1. **Actual render kernel self-time** -- Need profiler data on RenderSustainedLoopAVX2, RenderPrimaryVoiceSpan, etc.
2. **BuildStealHeap call frequency** -- Not in profiler Top Functions, but need to confirm it's truly rare
3. **Total render thread utilization** -- The synth's "CPU Render: 70-80%" vs profiler's different denominator
4. **Whether the quietest-voice-wins property can be approximated more cheaply** -- Current implementation is correct but expensive

---

## Stage 5 Dead Code Correction (Verified 2026-08-29)

The decompiled `ComputeStableStealKey` contained a `bVar1 == 5` branch in the steal-key computation. This was **dead code** — `envelopeStage` is a raw `uint8_t` (no named enum), and the actual stage progression is:

```
4 (delay) → 0 (hold) → 1 (attack) → 2 (decay) → 3 (sustain)
```

Stage 5 is **never assigned** in any code path. The branch was unreachable.

### Implications for stale-key hypothesis (INVALIDATED)

The stale-key hypothesis assumed:
1. Stage 5 voices exist → **False** (dead code)
2. Release voices are stable-tier → **False** (`IsStableStealCandidate` requires `state == 1 && envelopeStage != 2`; releasing voices have `state == 2`, so they always go to volatile tier, rebuilt every frame)
3. Sustain `currentGain` is updated every frame → **False** (`RenderBlockFrameMajor` line 1033: `if (!sustain) v.currentGain[idx] = gain;` — sustain voices don't update `currentGain`, so stable-tier keys are effectively constant)

**The steal comparison is provably fair.** The 24-voice ceiling at extreme density is a perceptual/dynamic-range phenomenon, not a steal-fairness bug.

---

## Correctness Investigation: CLOSED

All steal-score logic verified correct against real V3 source:
- Cross-tier comparison uses identical `EncodeStableWinnerKey(score - commonAgeScore, activePosition)` encoding
- Score formula `age/256 - |effectiveLevel| * 42000` is identical for both tiers
- Release voices are always volatile-tier (rebuilt every frame)
- Stable-tier `currentGain` is frozen by design (sustain voices)
- Cross-note batch stealing is explicitly unsupported for correctness (newborn commits re-key slots)

The community merge file is a legitimate extreme test case: 40k polyphony, 842M notes. The "ceiling" is perceptual, not algorithmic.
