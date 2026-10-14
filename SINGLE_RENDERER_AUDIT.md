# One renderer: audit, target, staged plan

Session 1 deliverable (2026-10-14). Read-only audit, no code touched. Branch `v3`.
Measured with `build\V3\svms_v3_bench.exe` (built 16:34 today, newer than `SVMSRenderScalar.h`
at 16:32, so it matches the working tree), AVX2 unless stated, 2048 frames, 6 render threads, 3 s.

## 1. Headline findings

1. **The whole-voice (WV) renderer already is the renderer.** Every bench workload I ran
   (`chopped-notes`, `sustained`, `dense`, `steal`, `note-burst`) executed 100% on WV
   (`wv_segments.kernel` ≈ all calls, `dense_rendered` 0, `spans` 0). The workload named
   `dense` is not the dense *planner*; it is WV too. Dense and sparse run only after WV refuses.
2. **WV refuses only a short list**, and it is the real cost of having three paths:

   | WV refuses when | Production-reachable? | Falls to | Measured cliff |
   |---|---|---|---|
   | `voices.v.rot != nullptr` (phase rotation modes 1-4) | yes (config) | dense/sparse | AGENTS.md: ~9x slower render |
   | vibrato active (CC1/pressure) on **scalar or SSE2** backend | yes (forced-scalar, Celeron class) | sparse hybrid | **0.57 → 5.96 cycles/voice-sample (p50 4% → 47%)**, measured below |
   | event type `Reset` (also `AllNotesOff`/`AllSoundOff` RenderEvents, which are no-ops in the driver) | **yes: `midiOutReset`, GM SysEx reset** | dense or sparse | not measured; no bench flag emits it |
   | `dispatcher_ == nullptr && batchDispatcher_ == nullptr` | tests/bench only | dense/sparse | n/a |
   | scratch/handle/job/row-op/excite reservation fails (`ReserveVoiceCapacity` etc.) | OOM only | dense/sparse | n/a |

   That is all (`PlanWholeVoiceBlock`, `SVMSRenderScalar.h:4169-4300`). Unmapped CCs, bank select, program change,
   re-excite, merge forks, master SysEx are already WV-native.
3. **Sparse is the only place quality-tier decimation exists** (`ComputeDecimationStep`, only in
   `RenderBlockSparseRange`, gated by `!correctnessMode`, `SVMSRenderScalar.h:5488`). WV never decimates.
   So today the default path never decimates, and decimation fires only after a refusal.
   Deleting sparse deletes decimation: that is an **audible-behavior boundary (9)**, needs your decision (Q2).
4. **Canonical engine is not a viable replacement** (section 4). It is also not "one renderer": it is a second one.
5. **SSE2 backend gives nothing**: scalar and SSE2 are identical on the reference workload
   (1.130 vs 1.131 cycles/voice-sample). `SVMSRenderKernelsSSE2.cpp` (133 lines) delegates the short spans to scalar and
   loops scalar for the rest.

## 2. Inventory

Line counts are `wc -l` of the current tree (they include working-tree edits).

### 2a. Engines / eras

| Engine | Lines | Called by | Unique support | Tests | Verdict |
|---|---|---|---|---|---|
| V3 product (driver + RenderScalar + VoiceManager + kernels + workers) | RenderScalar.h 5749, VoiceManager.h 5968, KernelsAVX2 2242, Kernels.cpp 551, Workers 848 (+647 posix) | winmm/KDMAPI/native/offline/BASS shim | everything | svms_v3_correctness (7367-line suite), growth, limiter, midi_stream, song, smokes | **keep, it is the target** |
| Canonical (`src/V3/Engine/*`) | 3580 (+456 tests, +bench) | only `svms_v3_render --synth canonical` (offline) | AVX2 across independent voices, deterministic tiles, zero-alloc | `svms_canonical_engine` (1 ctest) | see 4; recommend retire |
| `StandaloneSynth` | 955 | `svms_v3_render`, native offline API, Linux daemon, BASS shim | its own copy of the driver's event semantics (collapse, merge, re-excite, CC handlers) around the **same** `RenderScalar` (`renderer_.RenderBlock`, `:291,:402`) | indirect via `svms_v3_native_api`, song tests | keep the facade; dedupe the dispatcher (stage 7) |
| GPU (`SVMSGpuSynth.cpp` 611 + `SVMSGpuDevice.cpp` 239, `SVMSGpuSpike.cpp` 399) | ~1250 | `StandaloneSynth` line 284 when backend = GPU (non-XP, offline) | experiment | none in ctest (`svms_v3_gpu_spike` is a manual exe) | **ask** (Q4): delete or park |
| V1 TSF (`src/tsf.h` 5020, `TsfEngine.cpp` 844) and V2 BASSMIDI (`BassMidiEngine.cpp` 644) | ~6500 | `src/Synth.cpp` via root `build.bat`, **not** the V3 CMake | old product line | none | not part of this project; **ask** (Q5) |

### 2b. Render paths inside `RenderScalar` (`SVMSRenderScalar.h`)

| Path | Lines (approx.) | Entered when | Unique | Refuses / limits | Tests | Measured |
|---|---|---|---|---|---|---|
| **WV** `PlanWholeVoiceBlock`, `RenderWholeVoice*`, ghosts, hooks (`:3732-5473`) | ~1740 | default | steal ghosts, row ops, deferred release, re-excite, fork, per-channel limiter bus, AVX2 vibrato | the list in 1.2 | `TestWholeVoice{Steal,Launch,CC,Vibrato}Differential`, `TestPerChannelLimiter`, merge/re-excite tests | chopped 0.496 cyc/vs (p50 3.46%), sustained 0.408, dense 0.405, steal 0.413, note-burst 0.504 (AVX2); chopped scalar 1.130 |
| **Dense planner** (`ComputeDenseChunkMask` … `RenderBlockDensePlanned`, `:2325-3557`) | ~1230 | after WV refusal, no vibrato, ≥2 workers, no merged voices, no re-excite, adaptive gate open | chunk tiles; untouched voices get one full-chunk span | refuses merge/re-excite/vibrato/count>1 events; own shadow state `denseRenderState_` | `TestDensePlannerOracleDifferential`, `TestDenseProductionGateParity`, `TestRenderBackendSelectionAndDenseEquivalence` | **not measurable with the current bench** (nothing reaches it); needs a `--force-path` or a Reset workload (stage 0) |
| **Sparse span renderer** (`RenderBlockSparseRange`, `:5474-5749`) | ~275 (+ `RenderPrimaryVoiceSpan` 578 and `RenderStealTail*`, which WV also uses) | universal fallback; the only vibrato path on scalar/SSE2; the only place with decimation | decimation tiers | none (it is the fallback) | covered via oracle differentials | scalar + CC1 vibrato: **5.959 cyc/vs, p50 46.97%**; scalar + unmapped CC: 1.139, p50 7.84% (stays on WV) |
| **Oracle** `RenderBlockReference` + `RenderBlockFrameMajor` (`:1297-1661`) | ~360 | tests only (`SVMS_ENABLE_REFERENCE_RENDERER`) | the correctness gate | n/a | everything | n/a |

Shared helpers that are *not* deletable with dense/sparse: the class kernels, `RenderPrimaryVoiceSpan` (per-voice scalar,
the kernel fallback contract), `RenderStealTail*`, `VoiceManager`.

### 2c. Kernel backends

| Backend | Lines | Unique | Verdict |
|---|---|---|---|
| Scalar (`SVMSRenderKernels.cpp`) | 551 | bit-exact reference, boundary 2 | keep |
| SSE2 | 133 | nothing (delegates; measured identical to scalar) | fold into scalar (enum value stays as an alias) |
| AVX2 | 2242 | time-lane + voice-batched kernels; 2.3x scalar on chopped | keep |
| GPU | ~1250 incl. device | experimental | Q4 |
| AVX-512 | n/a | `RenderBackend::AVX512` is only the "auto" sentinel | n/a |

### 2d. Features that currently depend on which path runs

| Feature | WV | Dense | Sparse | Canonical |
|---|---|---|---|---|
| steal ghosts, 64-frame tail | yes | own capture | yes | no |
| merge forks, re-excite | yes | **stands down** | yes | no |
| per-channel limiter buses | yes | yes | yes (short AVX2 kernels refuse) | no |
| vibrato | AVX2 only | no | yes (all backends) | yes (own model) |
| phase rotation / Hilbert pair | **no** | no | yes, scalar spans only | no |
| decimation tiers | no | no | yes | no |
| Reset event | **no** | no | yes | n/a |

## 3. Target decision: Option A (WV becomes the only render path)

Reasons, with numbers:

- WV is already faster than anything else on every workload it can take: 0.41-0.50 cyc/vs at 5k-8k voices.
- Dense and sparse exist only to cover five refusals. Each refusal can be turned into a WV capability, and none of
  them needs the dense planner's chunk-tile machinery (1230 lines + a shadow copy of the voice state).
- A is the only option that satisfies the standing boundaries as written (1 timing, 2 scalar bit-identical, 5 no
  tolerance loosening, 9 no audible change) without a listening-test campaign.

Design for the five refusals (each is "teach WV", not "add a path"):

1. **Barrier events (Reset, and any future unknown type).** Split the block at the barrier frame: WV renders
   `[0, F)`, the barrier executes, WV renders `[F, N)`. Event order and frames are untouched (boundary 1). This
   replaces every future `default: return false` with one mechanism, so refusal stops existing as a concept.
2. **Rotation.** WV worker segments route `rot != nullptr` rows to the existing scalar `RenderPrimaryVoiceSpan`
   (which already carries the pair-form math). `CopyGhostRow` must carry the rot fields (ghosts currently carry none:
   `PlanWholeVoiceBlock:4172`). Result: the same audio as today, minus the dense/sparse planning cost. Staged item
   "AVX2 Hilbert-pair lane path" (AGENTS.md) then speeds it up later; it is independent.
3. **Scalar/SSE2 vibrato.** The scalar segment kernel rebuilds the 64-frame LFO windows exactly as the sparse hybrid does.
   Must be bit-identical to sparse (boundary 2): sparse is the oracle for it until deleted.
4. **Dispatcher-null and reservation failure.** Dispatcher-null is a test-only configuration: make it an assert. Reservation failure
   becomes "drop this block's new notes, keep rendering" or a hard error; today it silently reroutes to a slower path
   with different code, which hides OOM. Needs your OK (Q3).
5. **Decimation.** Not carried over unless you say so (Q2).

Net deletions at the end: dense (~1230) + sparse (~275) + SSE2 kernel (133) + adaptive gate/shadow state + 3 dense tests.
Roughly **-1700 lines of engine, one entire test axis**, zero new paths.

## 4. Canonical engine: do not promote

- **Speed (indicative, workloads are not equal):** `svms_canonical_engine_bench` AVX2 ≈ 300 M voice-samples/s
  serial, ≈ 1.09 G/s with 3 workers on one looped sine; V3 WV does 5.9-7.1 G/s on 6 threads (≈1.0-1.2 G/s *per thread*).
  Per thread V3 is about 3x. The canonical bench has no steal/churn workload because Canonical has no stealing.
- **Audio:** its own migration doc records legacy vs canonical correlation 0.00113 on `X-Cool 3! 31edo F1.mid`
  ("a real semantic/coverage difference"). Phase 3 closed filter, LFO, mod envelope, exclusive class, `sampleModes=3`
  but still lacks stacks/routes, steal ghosts, per-key cap, merge, re-excite, per-channel buses, 31-EDO, rotation and reverb
  on the offline path. Ordinary 7-bit MIDI only (rejects the SVMS extended keys, 3052 events on that fixture).
- **Boundaries:** promoting it changes audio (boundary 9), breaks scalar bit-identity with today (2), and is a
  total conversion of storage/calling conventions, i.e. the "partial conversion" risk (6), because the XP build
  already excludes it.
- It is not a simplification: it adds 3.6k lines and a second sample bank, second SF2 loader and second channel
  interpreter, and V3 would have to keep running during the A/B anyway.
- Recommendation: park (leave in tree, off by default) until stage 6 lands, then delete with the owner's OK (Q1).
  Reusable ideas (zero-alloc test pattern, deterministic tile reduction) can be lifted from it before deletion.

## 5. Staged plan

Gate for every stage (AGENTS.md boundary 8): build with `-j 2`, `ctest --test-dir build\V3` all green
(16 tests incl. canonical; asio stress skips), XP x86 build + tests when a shared header changes,
bench before/after with identical flags (reference chopped-notes p50 ≈ 3.4%, 0.49 cyc/vs), changelog entry,
one commit per stage, nothing pushed. Each stage is independently revertable.

| # | Stage | Audible? | Boundaries brushed | Exit criteria |
|---|---|---|---|---|
| 0 | **Instrument.** `lastRefusalReason` counters in telemetry (type already partly recorded: `lastRefusalType_`); bench flags `--force-path wv|dense|sparse` and a `--reset-rate N` workload; baseline dense/sparse numbers | no | none (test/bench only) | I can quote dense and sparse cycles/vs; production reports per-reason refusal counts |
| 1 | **Barrier splitting** in `RenderBlock`; Reset handled by WV | no (same frames, same order) | 1 | new differential: note storm + Reset at random frames vs oracle, scalar 1e-8-class, AVX2 ≤2e-5; Reset no longer reaches dense/sparse |
| 2 | **Rotation in WV** (scalar span per rot row, ghosts carry rot) | no, same audio as today | 1, 2, 4 (store padding) | `TestHilbertPairDifferential/LoopAware` unchanged; add WV-vs-oracle rotation storm with steals; rotated chopped-notes speed vs today (target: no worse; expected much better) |
| 3 | **Scalar/SSE2 vibrato in WV** | no, bit-identical to sparse | 2, 3 | WV vibrato differential on scalar must match sparse output **bit-for-bit** (new test runs both); the 5.96 cyc/vs cliff gone |
| 4 | **Refusal becomes unreachable:** planner returns bool only for reservation failure; dispatcher-null asserts; production counter proves 0 sparse/dense blocks over a real playthrough (DYHTM at pool 80k) | only if Q3 chooses drop-notes-on-OOM | 6 | counter = 0 for the playthrough |
| 5 | **Delete dense planner** + shadow state + adaptive gate + `denseLastCallbackMarked_` + the 3 dense tests + dense config/telemetry fields (keep ABI slots, zero them) | no | 6 (every consumer in one commit), runtime-link ABI | grep clean in `src/V3`, ctest + XP green, bench unchanged |
| 6 | **Delete sparse hybrid + decimation tiers** (keep `RenderPrimaryVoiceSpan`, tails, oracle) | **yes if you opt out of decimation (Q2)** | 9 | same as 5; `kDecimationTier*` and the `correctnessMode` decimation meaning removed or documented |
| 7 | **Share event semantics:** extract the driver's collapse/merge/re-excite/CC handlers into one header used by `Driver` and `StandaloneSynth` | no | 1 (order), 6 | offline vs realtime differential on a note-storm MIDI is byte-identical before and after |
| 8 | **Fold SSE2 into scalar** (enum alias; dispatch table loses a row) | no (SSE2 output already equals scalar path) | 2 | SSE2-only CPU emulation (`--backend sse2`) renders byte-identical to `--backend scalar` |
| 9 | **Retire Canonical** (and, per Q4, GPU) | no (offline `--synth canonical` disappears) | none | `svms_canonical_engine` ctest removed, CMake target removed, XP unaffected |
| 10 | Optional: AVX2 Hilbert-pair lane kernels (existing "staged next session" item 1) | no (≤2e-5) | 2, 3, 7 | rotated render speed target set after stage 2 numbers |

Order rationale: 0 first so every later claim has a number; 1-4 add capability while old paths still exist as the oracle
for each; 5-6 are pure deletion after their replacement is proven; 7-9 are independent cleanups that can run in any order after 4.

## 6. Questions for you

- **Q1 Canonical:** park now and delete at stage 9, delete now, or keep as an offline-only experiment? (Recommended: park, delete at 9.)
- **Q2 Decimation tiers (audible):** WV never decimates today and sparse is the only place it exists. Drop it (recommended: it
  already does not run on the default path), or port it to WV? Porting means a per-voice skip pattern inside WV items; that
  touches exact-frame semantics, so I would not do it without a spec from you.
- **Q3 Out-of-memory reservation failure in the planner:** drop that block's new notes and keep rendering (recommended),
  render silence, or abort? Today it quietly switches to a different renderer.
- **Q4 GPU path (~1250 lines, offline only, no ctest):** delete in stage 9, or keep?
- **Q5 `src/` V1/V2 tree (TSF, BASSMIDI, `Synth.cpp`, root `build.bat`, ~6500+ lines in `src/*.cpp`):** is it still shipped?
  It is outside the V3 CMake, so it does not block this plan; deleting it is a separate repo-hygiene call.
- **Q6 Fixture:** may I add a Reset-heavy and a rotation-heavy bench workload and run on the DYHTM merge for the stage-4 counter?

## 7. Risks I could see now

- Stage 3 bit-exactness is the hardest: sparse's vibrato LFO window advance is interleaved with exact-frame span boundaries;
  WV segments split at different frames (start/release/death). The LFO rebuild must be keyed on absolute frame, not
  segment start. If it cannot be made bit-identical, that is a boundary-2 conflict and I stop and ask.
- Stage 2 ghosts: a stolen rotated voice's tail needs its rotation state (angle, sweep phase, pair store offset)
  in `CopyGhostRow` (the existing comment: add any cold field the ghost path reads).
- Runtime-link/telemetry ABI exposes `render_paths` bits (0x1 WV, 0x2 dense, 0x4 sparse, 0x100 vibrato): keep the field,
  stop setting 0x2/0x4, so the configurator and `svms_v3_telemetry_abi_smoke` stay valid.
- `SVMSRenderEvent.h` still says "Only the exact span renderer models [NoteReExcite]"; that is stale (WV models it).
  I will fix the comment in stage 0.
- The test suite is large and `main()` runs only the newest tests (AGENTS.md); deleting dense also means auditing the
  dormant tests for references so a "dormant" test does not silently stop compiling.
- Working tree is dirty (30 modified files, note-merge work uncommitted). Stage 0 should start from a commit of that work
  or on top of it deliberately; I did not touch any of it.

## 8. What I did not measure

- Dense planner and sparse speed in isolation (unreachable from the current bench; stage 0 fixes that).
- Rotation cost today (quoted ~9x from AGENTS.md; no bench flag).
- Canonical vs V3 on an identical workload (different benches, different sample material).
- XP 32-bit numbers (not rebuilt; no shared header changed).

## 9. Owner decisions (2026-10-14, after review)

- Q1 Canonical: stays, offline-only, marked deprecated. Stage 9 no longer deletes it. XP is out of scope.
- Q2 Decimation: PORT to the whole-voice renderer as a toggleable option (config key, default = today's behavior on the default path, i.e. off). Needs a spec for exact-frame semantics before stage 6.
- Q3 Reservation failure: make it impossible at block time (reserve at init / pool change, bounded row-op cap); failure becomes a visible init/pool-resize error, never a silent renderer switch.
- Q4 GPU: keep, mark deprecated; owner wants realtime GPU rendering later.
- Q5 V1/V2 `src/` tree: keep untouched (historical).
- Q6 New bench workloads (reset, rotation): only after rotation is confirmed to use the CORRECT analytic (Hilbert) implementation.
- Vibrato and phase rotation must never refuse. Already the plan (stages 2, 3); they are requirements, not best-effort.

## 10. Stage 0 done (instrumentation, uncommitted)

Added `RenderScalar::GetPathBlocks/GetRefusalCount/SetForcedRenderPath`, bench `--force-path wv|dense|sparse`,
bench stderr line `{"paths":..., "refusals":...}`, fixed the stale NoteReExcite comment. No audio path changed.
Measured (AVX2, 6 threads, 2048 frames, 88 blocks):

| Workload | forced wv | forced dense (what actually ran) |
|---|---|---|
| chopped-notes 5000 | 0.498 cyc/vs, p50 3.5% | 4.007 (sparse 88/88), p50 35.3%, **8x slower** |
| dense 8192 | 0.412, p50 4.8% | 7.511 (dense 1, sparse 87), p50 83%, **18x slower** |
| mixed-events 8192 | 0.957, p50 10.4% | 13.390 (dense 1, sparse 87), p50 104%, **14x slower** |
| sustained 8192 | 0.405 | 0.418 (sparse; no events, equal) |

The dense planner ran in 1 of 88 blocks even when WV was skipped: its adaptive gate stands it down almost immediately.
Dense is effectively dead code in practice; sparse is the real fallback and is 8-18x slower than WV on event-bearing material.

## 11. Stage 1 done (uncommitted)

`RenderBlock` is now prologue + barrier split + `RenderBlockStretch` (the old body). Barrier = Reset, AllNotesOff, AllSoundOff,
unknown types (`IsBarrierEvent`). Same-frame events up to and including the barrier dispatch via the same dispatcher call
sparse makes, with blockCursor = the full-block frame. Stretches are rebased copies (`splitEvents_`, grows once, then reused).
`lastRenderPaths_` gains bit 0x200 for split blocks. Test `TestWholeVoiceResetBarrierDifferential` covers mid-block, frame 0 twice
then a note, last frame, no-op + reset same frame, lone no-op. The per-channel-limiter test that used AllSoundOff to force sparse now uses `SetForcedRenderPath(3)`.
Known cost: barrier blocks pay one extra plan/post per stretch; no `--reset-rate` bench workload yet.

## 12. Stage 2 done (uncommitted): rotation in the whole-voice renderer

- Plan no longer refuses on `v.rot`. `wvHilbertData_` carries the companion store into segments, batches, ghost tails and real tails.
  Ghost rows copy `rot[]` (`CopyGhostRow`), `WholeVoiceGhostTail` carries a `VoiceRotationState`, per-job tail scratch gets
  `ReserveRotation(1)`, adoption copies it into `stealTailRot`. `SyncWholeVoiceRotationScratch` allocates/releases the mirrors as the pool's
  rotation toggles (a stale ghost `rot` would rotate a coherent voice).
- **Root cause found by the first test run:** an in-place steal does not reseed rotation; the successor inherits the victim's angle at the
  steal frame. The pre-pass only holds the block-start copy, so successors were off by the victim's rendered samples before the steal
  (~150 steps, 0.005 rad, 6e-5..6e-4 audio, growing with sweep). Fix: `AdvanceVoiceRotation` advances the live slot by (steal frame - victim start) in the steal hook.
  Open design question for you (not changed): a stolen successor inheriting the victim's angle/sweep direction is arguably unintended;
  reseeding on steal would simplify this, but it changes audio, so I kept the oracle behavior.
- Mode 3 (Diffuse): the allpass memory `z0..z3` also inherits; it cannot be advanced in closed form. Not in the differential.
- Tests: `TestWholeVoiceRotationStealDifferential`, `TestWholeVoiceRotationLaunchDifferential` (modes 1/2/4, scalar+AVX2, ~1e-8 vs the oracle, all blocks on the whole-voice path;
  the launch variant uses all-looping long-release voices because rotation seeds depend on the slot handle and whole-voice retirement recycling differs by design).
  The two older pair-rotation storm tests drive a raw `SetVoice*` dispatcher that only the span renderer models; they now pin `SetForcedRenderPath(3)` (revisit at stage 6).
- Perf (interleaved, AVX2, 6 threads, chopped-notes 5000, 3 rounds): rotation modes 1/2/4 sparse (the old behaviour) 10.0-10.2 cyc/vs p50 ~77% vs whole-voice 2.16-2.35 p50 ~16%.
  Non-rotated chopped-notes base(stage1) vs new: 0.500 vs 0.508 median of 5 (one 0.584 outlier on the new side was a GTNH spike).
- Saved baselines: `build/V3/svms_v3_bench_base_stage1.exe`, `..._base_stage2.exe` (bench-local, untracked under build/).

## 13. Stage 3 done (uncommitted): scalar/SSE2 vibrato in the whole-voice renderer

- Whole-voice no longer refuses on vibrato for any backend. Scalar/SSE2 blocks (`wvScalarVibrato_`) split each voice's segment at the sparse loop's
  window boundaries (`wvLfoBounds_`: every event frame, plus every 64 frames within a gap) and call `AdvanceVibratoSpanRow` per window.
  `AdvanceVibratoSpan` (sparse) now calls the same row function, so there is one definition of the math (advance-then-use, powf ratio, delay, restore).
- Exactness: the old sparse loop samples "any channel modulated" only at block start (a CC1 arriving mid-block in an unmodulated block does nothing until
  the next block). Kept deliberately (`wvScalarNoVibrato_`) so forced-scalar audio stays what it was.
- Rows with `vibLfoSteps == 0` render as one segment (windows cannot touch them). That is where the big win is.
- Test: `TestWholeVoiceScalarVibratoDifferential` (6 blocks, scalar whole-voice vs forced sparse, 8e-9). Blocks 6-7 of the same scenario diverge by design
  (saturated pool: whole-voice scores steal victims at block start and retires at block end), so the scalar-exact run stops before the pool saturates.
  The AVX2 vibrato test now pins its reference to forced sparse (it silently turned into scalar-whole-voice otherwise).
- Perf (scalar, chopped-notes 5000, 16k notes/s + 4k CC1/s): no-LFO voices 6.2 -> 1.3 cyc/vs (p50 47% -> 9-10%); LFO voices (`--vibrato-lfo`) sparse 12.5-12.9 vs
  whole-voice 10.9-12.9, noisy (GTNH running); no regression, likely small gain.
- Not covered: SSE2 has no dedicated test (shares the scalar kernels). Diffuse/rotation + vibrato together not tested.

## 14. Stage 4 done (uncommitted): refusal is unreachable

- `PrepareWholeVoice(voices)` front-loads every reservation the plan used to make lazily (scratch, per-handle timelines, per-job scratch, op log at the
  64k-op budget, re-excite lists, rotation mirrors). Called from `Driver` init, `StandaloneSynth`, and the offline renderer; failure is an init error.
  The plan still calls the same ensure functions (early-outs), so a later pool change just re-reserves.
- `WholeVoiceRowOpEstimate` is the single per-event op count (plan and splitter share it). Blocks whose op total would exceed `kWvRowOpBudget` are cut at the start
  of the offending frame's event group (kind 2 split, no dispatch). One frame holding more than a whole budget of ops (4096+ master SysEx at one frame) keeps the group
  whole and the log grows: the only remaining allocation path, pathological.
- Dispatcher-less render: events ignored (matches the sparse loop), no refusal. Plan `default:` (unknown event type) asserts; barriers are split out before it.
- Proof: 33 bench combos, all 100% whole-voice with every refusal counter zero. Not run: the DYHTM playthrough you postponed. `reservation` can still count if a pool
  GROWS after init and the grown handle capacity is not yet reserved; it falls back to the old paths until stage 5/6 remove them (then it needs the Q3 policy).
- Remaining refusal sources until stage 5/6: that late-growth reservation, and OOM.

## 15. Stage 5 done (uncommitted): dense planner deleted

- Removed from `SVMSRenderScalar.h` (5749 -> 4430 lines): `ComputeDenseChunkMask`, `AdvanceAuthoritativeSpan`, `AdvanceDense*`, `Dense{PreTailCapture,VoiceConfigured,VoiceReleased}`,
  `RenderDenseVoiceTile`, `RenderDenseTails`, `DenseIndexedJob`, `RenderBlockDensePlanned`, `EnsureDenseStorage`, the snapshot/mutation/plan/job structs, the shadow state and
  mark arrays (also gone from `ReserveVoiceCapacity`, so renderer scratch per voice shrank), constants and reject/coverage counters. Dense-only `VoiceSoA` mode removed
  (`ReserveDenseRender`, `CopyRenderProgress`, `CopyDenseRenderStateFrom`, `denseOnly_`); `SVMS_VOICE_SOA_DENSE_FIELDS` stays (ghost rows use that field set).
- `RenderBlockStretch` now ends in a single `RenderBlockSparseRange` call over the stretch (still reachable only with `SetForcedRenderPath`/a plan fallback; stage 6).
- Driver: the memory-budget search no longer has the dense step at 8193 voices (`LargestInitialVoiceCapacityForBudget` is one search; the "dense renderer" budget fallback
  now applies at every pool size).
- Kept on purpose: `RenderSpanContext::handleBase` and the AVX2 aligned-load branches (dense-tile mode) are now unused by production; remove with the AVX2 kernel pass.
  `pathBlocks_[1]`/render_paths bit 0x2 stay (always 0) for ABI/telemetry stability. Stale comments still say "dense" in VoiceManager.h, DriverRender.cpp, StandaloneSynth.h,
  ARCHITECTURE.md/performance docs; sweep them when stage 6 lands. AGENTS.md still describes the dense planner (owner file, untouched).
- Tests: `TestDensePlannerOracleDifferential` (dormant) deleted; the others pass. ctest 16/16. Bench: no change within noise.

## 16. Stage 6 (revised, uncommitted): decimation ported, span renderer made test-only

- **Finding that changes the premise:** the sparse span renderer never decimated. It only sorted `activeList_` by velocity; the class kernels mixed every voice. The real cull
  (mix only the loudest `activeCount/step`, advance the rest silently) existed only in the frame-major oracle. So there was no production decimation behaviour to preserve, and
  "port it how sparse did it" means porting the oracle's rule.
- Implemented in the whole-voice renderer, opt-in: `quality.decimation` (JSON, default false, restart-applied), `RenderScalar::SetDecimationEnabled`. Tiers 10k/25k/50k/150k,
  step 2/4/8/16. Rank by velocity (ties by handle) once at block start with `nth_element`; muted rows skip the class kernels and render through `RenderPrimaryVoiceSpan`
  with mixedFrameCount 0 (phase/envelope/retirement continue); rows launched in the block always mix; batches are off while any row is muted.
  Oracle now follows the toggle too (it used `correctnessMode`). Telemetry's decimationStep reports the real step.
- `TestWholeVoiceDecimation`: static 40-voice pool with tiers shrunk to step 4; audio vs oracle 1.9e-9 (scalar) / 1.0e-7 (AVX2), disabled == untouched audio exactly,
  muted voices keep advancing (phase within 0.5 sample: a muted span adds the phase in one step so float rounding drifts; envelopes exact).
- Perf, sustained 30000 voices (step 4): AVX2 0.414 -> 0.131 cyc/vs (p50 20% -> 6%); scalar 1.056 -> 0.276 (47% -> 13%).
- **What was NOT deleted, deliberately:** `RenderBlockSparseRange` (+ `AdvanceVibratoSpan`) stays, but only under SVMS_ENABLE_REFERENCE_RENDERER. Three kinds of tests still need it as a reference:
  the scalar-vibrato exactness test (sparse is the definition of today's forced-scalar vibrato), the two pair-rotation storm tests (raw `SetVoice*` dispatcher), and the per-channel-limiter
  odd blocks. Production no longer contains it: a plan that cannot reserve (late growth/OOM) skips the stretch. If you want it gone entirely, those tests need golden references first.
- **Not done:** Configurator toggle and a live RuntimeLink command for decimation (the key is JSON/restart-applied only); `quality.decimation` is not shown in the UI yet.

## 17. Stage 6 finished (uncommitted): span renderer deleted, toggle live, kernel cleanup

- **Step 1:** `RLCommandType::SetDecimation` (0x11E) + `RenderScalar::decimationEnabled_` is now atomic; ConfigDocument `quality.decimation`; PagePerformance lever VOICE DECIMATION (live).
  The FULL CORRECTNESS tooltip now says it no longer changes live rendering. Not covered by a test (no runtime-link command tests exist for any command).
- **Step 2:** golden references captured from the span renderer right before deletion: `src/V3/tests/SVMSVibratoGolden.h` (8 blocks x 512 frames + final active list).
  `TestWholeVoiceVibratoDifferential` (AVX2, 3e-2 budget; measured 0.0175) and `TestWholeVoiceScalarVibratoDifferential` (6 blocks, 2e-5; measured 8.4e-9) compare to it.
  Removed: `RenderBlockSparseRange`, `AdvanceVibratoSpan`, `RenderCoverageStats` + accessors, `forcedPath_`/`SetForcedRenderPath`, bench `--force-path`, the two sparse-only pair-rotation sections,
  the PCL test's forced-sparse odd blocks (now all whole-voice, barrier on odd blocks). RenderScalar.h 5749 -> 4146 lines.
- **Step 3:** `RenderSpanContext::handleBase` removed (AVX2 sustained short kernel keeps its own >=75%-of-pool contiguous heuristic); `FilterVoices8AVX2` lost its handle-bias argument;
  ~45 stale dense/sparse/legacy comments rewritten (code, public svmsapi.h doc comment, AGENTS.md).
- **Still there, dead or nearly so (candidates for the next pass):** VoiceManager render-class lists (`LinkRenderClass`, `ForEachRenderClassBlock`, class-change scratch `classChanges_`, `retirements_`
  in RenderScalar) were maintained for the span renderer; the whole-voice path only uses `RefreshRenderClass` at block end. Removing them would also cut per-launch/retire work. `VoiceReleaseHook`
  and `GetMaxLaunchGroupSize` look dead. `render_paths` bits 1/2 and `GetPathBlocks(1/2)` always read 0 (kept for ABI). Muted decimation spans add phase in one step (drift <0.5 sample over a few
  blocks); `exactSilentAdvance=true` would make it bit-exact at a per-frame cost.
- Perf after the deletion (interleaved, AVX2, chopped-notes 5000, 6 rounds): 0.5005 vs 0.506 median for the stage-4 build: unchanged within noise.
