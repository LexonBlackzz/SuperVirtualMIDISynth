# Canonical Engine Migration

Status: Phase 2 offline canonical integration implemented (2026-09-18).

This document describes how to replace V3 synthesis without replacing the V3
host. No production synthesis behavior is changed by this phase.

## Phase 2 implementation boundary (2026-09-18)

The first host integration is the standalone `svms_v3_render` session only.
Its existing `MidiStreamDecoder` and `ParsedEventRing` remain authoritative:
the consumer renders up to `PackedMidiEvent::outputFrame`, then consumes every
equal-frame successor in ring order before advancing audio. A temporary
whole-session `legacy|canonical` selector chooses an offline-synth facade at
session creation. The legacy facade delegates unchanged to `StandaloneSynth`;
the canonical facade converts each packed channel message to a value-only
`CanonicalEvent { absoluteFrame, sequence, type, channel, data1, data2 }` and
submits it to `CanonicalEngine` in that same call order.

`CanonicalEngine` owns channel/program/RPN/sustain interpretation, prepared
SoundFont lookup, logical note instances, physical voices, scalar/AVX2
rendering, and synthesis telemetry. It exposes no V3 queue, scheduler,
planner, voice-manager, public-API, or audio-host types. The offline facade
applies the existing `LimiterRouterState` after either engine. The CLI did not
previously apply the driver's reverb, so this phase does not invent a different
offline reverb path; that deliberate compatibility gap remains documented
until the existing driver reverb can be mechanically shared and differentially
verified. Realtime `Driver::RenderCallback`, ingress, scheduler, reset fences,
and public ABI are outside this boundary and remain untouched.

## Architectural decision

The integration seam is the ordered block-event array produced in
`Driver::RenderCallback`, after V3 has:

1. accepted public API, KDMAPI, WinMM, and player submissions;
2. applied cancellation and reset-fence policy;
3. converted QPC or absolute-frame timestamps;
4. compiled and globally ordered events by `(targetFrame, sequence)`;
5. retained future events in the scheduler;
6. applied late-event and overload policy;
7. converted due events to block-local exact frame offsets; and
8. compacted only semantically consecutive same-key NoteOff runs.

Today this array is passed to `RenderScalar::RenderBlock`. The canonical path
will pass the same array, through a small type-only adapter, to one canonical
engine. The scheduler remains authoritative for *when*. The engine becomes
authoritative for channel interpretation, SoundFont region expansion, voice
lifecycle, and audio generation.

```text
existing V3 ingress/compiler/scheduler
        |
        | ordered ScheduledRenderEvent/RenderEvent values
        | exact block-local frameOffset + ingress sequence
        v
CanonicalEngineAdapter                 (new, no queue or clock ownership)
        |
        | CanonicalEvent values
        v
CanonicalEngine                        (new authoritative channel/SF2/SoA state)
        |
        | scalar or AVX2 lanes over the same state
        v
planar stereo or optional channel buses
        v
existing channel limiter -> interleave -> reverb -> master limiter -> host
```

The adapter must not include `SVMSVoiceManager`, render classes, planner jobs,
renderer eligibility, or kernel fallback state. `RenderEvent` must eventually
move out of `SVMSRenderScalar.h` into a host/engine-boundary header so the event
scheduler no longer depends on the legacy renderer.

The preferred engine call is one block operation rather than an exposed series
of mutable host calls:

```cpp
void CanonicalEngine::RenderBlock(
    uint64_t absoluteFrame,
    uint32_t frameCount,
    std::span<const CanonicalEvent> orderedEvents,
    CanonicalRenderTarget target);
```

Internally it follows V4's rule: render `[cursor, eventFrame)`, dispatch all
events at that frame in sequence order, recover event retirements between
events, and continue at the event frame. An event at the half-open block end
updates state without producing a sample.

## V3 components intentionally preserved

The following remain host-owned and are not redesigned:

| Area | Current implementation | Migration treatment |
|---|---|---|
| Public/native API routing | `SVMSDriver.cpp`, `SVMSAPI.h`, `include/svmsapi.h` | Preserve |
| KDMAPI and WinMM surfaces | V3 driver exports/facades | Preserve |
| External backend router | `Driver::ForwardBlockToBackend` and backend loading | Preserve; canonical selection applies only to the in-process sink |
| Producer queues | `SVMSMPSCQueue.h` and lane policy | Preserve |
| Timestamp compilation | `SVMSEventCompile.h`, `SVMSFrameClock.h` | Preserve |
| Event compiler pages | `SVMSEventPages.h`, compiler thread | Preserve |
| Ordered scheduler | `SVMSEventScheduler.h` / paged scheduler | Preserve |
| Late-event, shedding, and cancellation policy | `Driver::RenderCallback` admission logic | Preserve |
| Reset/termination fences | V3 producer and dispatch fence logic | Preserve at the adapter boundary |
| Callback pacing and virtual frame clock | `Driver::RenderCallback` | Preserve |
| WASAPI / ASIO / DirectSound | V3 audio outputs | Preserve |
| Runtime link and configurator | existing V3 implementation | Preserve |
| Player and application integrations | existing V3 clients | Preserve |
| Reverb | current V3 `ReverbState` behavior | Preserve, then extract without DSP changes |
| Master limiter and high-pass | `SVMSLimiter.h` and current ordering | Preserve |
| Per-channel limiter | `SVMSChannelLimiter.h` | Preserve; canonical engine must support optional channel-bus output |
| SoundFont file selection, stacks, routes, reload publication | V3 configuration and bundle lifecycle | Preserve at the host boundary |

The scheduler/event implementation needs only dependency cleanup: it should
include a neutral boundary event header instead of `SVMSRenderScalar.h`. No
sorting, queue, timestamp, pacing, cancellation, or admission algorithm needs
to change.

## Current V3 synthesis ownership

The active in-process synthesis path currently spans:

- `SVMSDriver.cpp`: channel/SoundFont dispatch, prepared launch plans,
  SoundFont bundle ownership, live DSP configuration, render orchestration,
  reverb implementation, and telemetry;
- `SVMSVoiceManager.h` and `SVMSTypes.h`: authoritative legacy voice SoA,
  stealing, class lists, channel/key/exclusive-class indices, ghosts, and
  lifecycle;
- `SVMSRenderScalar.h`: exact-event coordinator plus whole-voice, dense,
  sparse, reference, and fallback renderer architectures;
- `SVMSRenderKernels*.cpp`: scalar, SSE2, and AVX2 class kernels;
- `SVMSRenderWorkers.*`: legacy renderer work distribution;
- `SVMSStandaloneSynth.h`: a second host facade around the same legacy
  synthesis machinery, used by offline/native/Linux paths;
- `SVMSSoundFont.*`: parser and mutable/raw SF2 representation used to build
  prepared launch data.

These components remain available as the whole-application `legacy` A/B engine
during migration. None may be called per voice from the canonical engine.

## V4 concepts to reuse

Import engine concepts, not V4's experimental host:

| V4 source | Reuse |
|---|---|
| `include/midisynth/synth.h`, `src/synth.cpp` | Single event-span coordinator, authoritative SoA, numeric slots, active/free arrays, per-note intrusive lists, deterministic retirement |
| `src/avx2_kernel.*` | AVX2 across independent voices through a non-owning view of the authoritative SoA |
| `include/midisynth/sample_bank.h`, `src/sample_bank.cpp` | Immutable numeric sample descriptors; adapt storage to V3's exact int16 load conversion or prove an alternative |
| `include/midisynth/soundfont.h`, `src/soundfont.cpp` | Immutable prepared-region/preset lookup model and SF2 DAHDSR/key-scaling semantics |
| `src/channel_interpreter.*` | Channel/controller and logical key-instance semantics, but not its allocation-capable vector output API on the realtime thread |
| V4 tests | Exact-frame, ordering, lifecycle, scalar/AVX2, threading, and allocation-test patterns |

Do not import V4's realtime event queue, realtime session, WASAPI host, public
API implementation, SMF host, or WAV application shell into the V3 runtime.

The imported module should live under `src/V3/Engine/` and use V3 project
namespaces/naming. A proposed initial layout is:

```text
src/V3/Engine/
    SVMSEngine.h
    SVMSEngine.cpp
    SVMSEngineTypes.h
    SVMSEngineEvent.h
    SVMSEngineSoundFont.h
    SVMSEngineSoundFont.cpp
    SVMSEngineState.h
    SVMSEngineScalar.cpp
    SVMSEngineAVX2.cpp
    SVMSEngineWorkers.h
    SVMSEngineWorkers.cpp
    SVMSLegacyEngineAdapter.h
    SVMSCanonicalEngineAdapter.h
```

The exact split may change, but the coordinator, SoundFont preparation,
backend kernels, and executor must not accumulate in `SVMSDriver.cpp`.

## Event mapping

The neutral canonical event needs at least:

```text
frameOffset, ingressSequence, type, channel, data1, data2, multiplicity
```

Mapping from V3 due events:

| V3 event | Canonical action |
|---|---|
| NoteOn | Resolve current canonical preset, create one logical note instance, launch all matching prepared regions |
| NoteOff | Release the oldest matching logical note instance, respecting sustain ownership |
| StaleNoteOffBatch | Repeat the same oldest-instance NoteOff transition `data2` times at the same frame without changing ordering around other events |
| ControlChange | Update canonical channel state; implement 0/32, 1, 6/38, 7, 10, 11, 64, 66, 98-101, 120, 121, and 123 as required |
| ProgramChange | Update canonical channel program/preset selection |
| PitchBend | Update canonical bend state at the exact frame |
| ChannelPressure | Update the canonical modulation source |
| MasterVolume | Update canonical master mix scale |
| MasterFineTune / MasterTranspose | Update canonical pitch terms |
| RhythmPart | Update canonical percussion/preset-selection state |
| Reset | Reset canonical voices and channel state; host-owned fences and post-effect resets remain host actions |

No event may be reordered in the adapter. Same-frame sequence remains the V3
ingress sequence. The canonical engine must not sort events supplied by V3; in
debug builds it should validate monotonic `(frameOffset, ingressSequence)`.

## SoundFont ownership and reload

V3 already has the correct publication shape: `BuildSoundFontBundle` performs
file I/O and preparation off the audio thread, `PublishSoundFontBundle` performs
an atomic handoff, and `ActivatePendingSoundFontAtBlockBoundary` swaps at a
callback boundary and retires the old immutable bundle off-thread.

Keep that lifecycle. Extend each bundle with a canonical immutable payload:

- combined numeric sample store/descriptors;
- flat prepared regions;
- precomputed preset key/velocity lookup;
- stack routing metadata or references needed by V3's existing route policy;
- no pointers from voice state into parser objects.

During A/B, a bundle may temporarily contain both legacy and canonical prepared
payloads. The canonical engine may not consult `SF2Data`, `SFSampleRegion`, or
legacy voice launch structures at render time. Once legacy is retired, the raw
legacy runtime payload can be removed while retaining V3's configuration,
stack/route, loading, and publication behavior.

V3 currently stores int16 samples plus eight trailing zero elements and converts
with `1.0f / 32768.0f`. The first canonical integration should retain this
storage/conversion contract. Converting every SoundFont to a second float bank
would double long-lived sample memory during A/B and would make parity analysis
harder. The canonical sample descriptor remains numeric and storage-agnostic,
so a later measured change is possible.

On activation, reset the selected whole engine because voice sample offsets
refer to the old immutable bank. Preserve channel/program state only if the
current V3 reload contract does; rebuild canonical preset selection at the same
block boundary.

## Canonical voice and execution model

The new engine owns one preallocated SoA. Required lifecycle structures are:

- numeric voice slots and generation/logical-note IDs;
- LIFO free-slot storage;
- dense active-slot storage with per-slot active position;
- per-channel/key intrusive lists;
- deterministic retirement queues;
- exclusive-class channel lists;
- deterministic capacity handling and Quality-policy stealing;
- fixed preallocated tile buffers and retirement scratch.

Scalar and AVX2 operate on this state. AVX2 is a lane-width implementation, not
a separate synth, and cannot reject a feature. Threading assigns the same tiles
to workers and reduces in deterministic tile order. Worker count cannot alter
event dispatch or lifecycle.

V4 currently drops NoteOn at capacity. That is inadequate for V3/Black-MIDI
compatibility. Quality stealing and its deterministic victim rules must be
implemented in the canonical lifecycle before realtime becomes the default.
Fast/scan legacy policies are not canonical requirements and should not shape
the new data model.

V3's normal build is C++20, but the XP target is C++17. V4's use of C++20
`atomic::wait/notify` therefore cannot enter shared engine code unchanged.
Separate the semantic engine/tile code from the executor:

- modern builds may use the V4-style persistent atomic-wait workers;
- XP may use a compatible persistent executor or run the same tiles serially;
- both use identical state, tile boundaries, kernels, reduction, and lifecycle.

SSE2 is not a required new synthesis backend. On an SSE2-only CPU the canonical
scalar kernel is the correctness path. This changes no semantics.

## Post-processing ownership

The current realtime order is:

```text
synthesis -> optional per-channel limit/sum -> stereo interleave
          -> reverb -> master limiter/high-pass -> recorder/output
```

Keep this order and behavior during integration. The canonical engine normally
produces planar stereo. When the V3 per-channel limiter is enabled, the same
canonical kernel must target preallocated per-channel buses; this is an output
accumulation mode, not a feature-dependent renderer. The existing
`ChannelLimiterState` then limits/sums those buses.

The current reverb implementation (`ReverbState`) is embedded in
`SVMSDriver.cpp`; the limiter is already largely isolated in `SVMSLimiter.h`.
Extraction should happen only after the canonical path is proven:

```text
src/V3/Effects/SVMSReverb.h/.cpp
src/V3/Effects/SVMSLimiter.h/.cpp
```

Extraction must be mechanical and covered by impulse/stream differential tests.
Realtime and offline paths should invoke the same post-processing modules with
the same configuration. Offline currently uses the limiter through
`StandaloneSynth`; the CLI path does not currently apply the driver's reverb,
so adding offline reverb is a deliberate integration item, not a claimed
existing parity property.

## NoteOn coalescing semantics

Current V3 `NoteOnCollapseGate` is an opt-in lossy overload policy:

- it uses a fixed 20 ms window (QPC in realtime, audio frames offline);
- the first hit spawns;
- inside the window, only every threshold-th hit spawns;
- suppressed hits increment a stack counter;
- the next spawned event gets `2 * floor(log2(stack))` velocity units;
- NoteOff does not clear the key window; CC120/123 and reset do;
- realtime performs this before ingress, while `StandaloneSynth` mirrors it
  during event dispatch.

This is not semantically equivalent to N NoteOns and cannot satisfy a
coalescing-enabled/disabled audio or logical-voice differential. It must not be
silently relabeled as exact coalescing.

Introduce a reusable event-stream transform above the canonical engine with
clearly separated modes:

1. `ExactGroup`: group only contiguous, semantically identical NoteOns at the
   same exact frame into `multiplicity=N`. This is valid for realtime and
   offline and must differential-test equal to ungrouped input.
2. `LegacyThrottle`: preserve the current 20 ms suppression/velocity-stack
   behavior as an explicitly lossy overload option while compatibility requires
   it. Its tests compare realtime and offline implementations to the documented
   legacy policy, not to uncoalesced audio.

Grouping cannot cross any intervening event, even an apparently unrelated one,
unless commutativity is separately proven.

`ExactGroup` has two independently testable execution policies:

- `Expand`: dispatch N logical voices in sequence at the exact frame without
  heap allocation. This is the simple correctness oracle.
- `Cohort`: keep one physical voice lane with `multiplicity=N` while all N
  logical voices have identical synthesis state. Its contribution is multiplied
  by N, so eight phase-aligned identical voices initially render as one lane at
  eight times the contribution. The engine still reports and accounts for eight
  logical voices.

The cohort must split or reduce multiplicity whenever members diverge. Examples
include one-at-a-time FIFO NoteOff, sustain ownership, exclusive-class choke,
voice stealing, capacity policy, per-note controllers, or any future feature
with per-instance state. Released members may become a separate cohort when
their release state is identical. A permanently fused “8x voice” that cannot
split is not multiplicity-preserving and, if retained for experimentation, must
be named and tested as an intentionally lossy stacked-voice mode.

`Expand` versus `Cohort` must preserve event timing, logical voice count,
layered-region count, capacity/steal decisions, retirement, telemetry, and audio
within the canonical scalar tolerance. Cohort compression is an optimization of
canonical state execution; it is not a renderer selection rule or a second
synthesizer.

## Feature compatibility inventory

This table is the gate for making `canonical` the production default. “V4”
describes the current V4 working tree, not an intended future capability.

| Feature | Legacy V3 | Current V4 | Canonical action |
|---|---:|---:|---|
| Exact frame events and stable same-frame order | Yes | Yes | Reuse V4 coordinator with V3-ordered input |
| Velocity curve `(velocity/127)^2` | Yes | Yes | Preserve |
| Constant-power region pan | Yes | Yes | Preserve and differential-test center/extremes |
| Channel CC7/10/11 | Yes | Yes | Preserve exact-frame response |
| Sustain and overlapping-key ownership | Yes | Yes | Reuse V4 logical instance model; add CC66 parity |
| Pitch bend and RPN 0,0 range | Yes | Yes | Preserve; add V3 master tune/transpose terms |
| CC120/121/123 | Yes | Yes | Preserve precise V3 reset/panic semantics |
| Program/bank/drum selection | Yes | Yes, simpler | Add V3 stack routes and rhythm-part behavior |
| Layered regions | Yes | Yes | Preserve atomic logical launch semantics |
| DAHDSR volume envelope | Yes | Yes | Preserve V4 SF2 constant-dB behavior and validate against files |
| Per-note hold/decay key scaling | Parser fields; launch support needs audit | Yes | Adopt V4 semantics and add direct tests |
| Root key, coarse/fine tune, scale tuning | Yes | Yes | Preserve |
| One-shot and continuous loops | Yes | Yes | Preserve |
| `sampleModes=3` release-loop exit | Yes | No (continuous-loop warning) | Implement in canonical loop mask/state |
| Low-pass filter (`initialFilterFc/Q`) | Yes | No | Add prepared coefficients and per-voice state to ordinary lane pipeline |
| Vibrato LFO pitch | Yes | No | Add LFO state/masks to ordinary lane pipeline |
| Mod wheel/channel pressure influence | Partial V3 default path | No | Specify V3 behavior and implement canonical modulation sources |
| Modulation LFO | Parsed, not generally rendered | No | Compatibility gap; implement only after behavior/spec audit |
| Modulation envelope | Parsed, not rendered | No | Compatibility gap; implement canonical state, never legacy fallback |
| Explicit SF2 modulators | Parser data exists; effective support requires audit | No | Inventory supported V3 subset, then implement or document |
| Default SF2 modulators | Partial/custom V3 behavior | Mostly no | Define intended compatibility set explicitly |
| Exclusive class / SFZ off_by | Yes | No | Add per-channel class index and deterministic choke |
| Forced key/velocity generators | Parser support requires audit | No | Implement in preparation if required by supported files |
| SoundFont stacks and routes | Yes | No | Keep V3 host policy, prepare canonical banks |
| SFZ | Yes in V3 parser path | No | Inventory supported V3 opcodes before production switch |
| Voice stealing and ghost tails | Yes | No | Add canonical deterministic Quality stealing; decide ghost-tail semantics by differential/listening tests |
| Per-key voice cap | Yes, optional | No | Add canonical lifecycle policy if retained |
| Per-channel limiter buses | Yes, optional post effect | No | Add canonical channel-bus render target |
| Phase rotation/Hilbert path | Yes, currently under active repair | No | Treat as a canonical feature only after its desired semantics and tests are stable |
| 31-EDO mode | Yes | No | Add host-prepared tuning term without alternate renderer |

The rows marked “audit” are not permission to fall back to legacy rendering.
They are blockers to production-default canonical mode until resolved or
explicitly accepted as compatibility gaps.

## Incremental implementation plan

### Milestone A: import an independently tested engine

1. Add `src/V3/Engine/` with V4-derived sample descriptors, events,
   authoritative SoA, scalar tile kernel, coordinator, and telemetry.
2. Keep the module independent of `SVMSDriver.cpp`, V3 API structs, audio
   outputs, schedulers, and legacy voice/render headers.
3. Port V4 core tests into a new `svms_canonical_engine_tests` target.
4. Add no-allocation render guards and exact frame/order/lifecycle tests.
5. Add AVX2 as a second lane implementation over the same SoA and run scalar
   differential tests at the existing tolerance; do not add AVX512/GPU/SSE2.
6. Add an executor abstraction that builds in both the normal and XP targets.

### Milestone B: prepare V3 SoundFonts for the canonical engine

1. Build canonical immutable sample/region/preset data off-thread inside the
   existing bundle load operation.
2. Preserve stack priority, explicit routes, percussion selection, reload
   publication, and sample padding.
3. Add parser/preparation tests for layering, ranges, tuning, DAHDSR, loops,
   and warnings.
4. Keep both payloads only while whole-application A/B requires legacy.

### Milestone C: offline canonical integration

1. Add a temporary whole-session selector: `legacy` or `canonical`.
2. Route `SVMSOfflineRenderer`, native offline sessions, and MIDI-song tests
   through a common engine facade.
3. Preserve existing event decoding and absolute output frames.
4. Share limiter and later reverb at the post boundary, not in voice kernels.
5. Add exact-group coalescing on/off differential tests: output length, source
   event count, represented event count, logical launches, peak voices, final
   lifecycle state, and audio.
6. Add `ExactGroup Expand` versus `ExactGroup Cohort` differentials covering
   simultaneous stacks followed by staggered NoteOffs, sustain/re-press,
   capacity pressure, stealing, layered regions, and controller changes.
7. Record intentional legacy/canonical semantic differences rather than
   loosening tolerances.

### Milestone D: complete required canonical features

Implement release-loop exit, filter, exclusive class, vibrato/controller
modulation, Quality stealing, stack/routing behavior, channel buses, and other
accepted compatibility requirements in the canonical state/kernels. Each
feature gets scalar semantics first, then AVX2 differential coverage. A feature
must never choose the legacy renderer or a different synthesis architecture.

### Milestone E: realtime integration

1. At the existing `RenderScalar::RenderBlock` call site, select the entire
   legacy or canonical engine for the callback.
2. Feed the canonical engine the already-admitted ordered event array.
3. Keep post effects and audio-host code unchanged.
4. Extend allocation guards to the canonical callback path.
5. Separate scheduler/admission timing from canonical synthesis timing in
   telemetry.
6. Validate public API, KDMAPI, WinMM, runtime link, WASAPI, ASIO,
   DirectSound/XP, Ziggy, and Black-MIDI playback.

### Milestone F: effects extraction and retirement

1. Mechanically extract reverb and limiter orchestration with audio
   differentials.
2. Make canonical the default only after correctness, listening, and workload
   gates pass.
3. Remove active references to legacy planners, renderer classes, SSE2/AVX2
   legacy kernels, and `SVMSVoiceManager` synthesis state.
4. Remove the temporary selector after a defined stabilization period.

## Validation gates

Every implementation milestone must run, as applicable:

- current V3 build and complete CTest suite;
- XP x86 build and tests when shared headers or targets change;
- V4-derived canonical scalar tests;
- canonical scalar versus AVX2 audio/state differential;
- canonical serial versus threaded byte-identical output/state differential;
- repeated threaded tests;
- render-time allocation test;
- offline exact-group coalescing differential;
- legacy/canonical offline audio reports using identical event streams and
  post settings;
- realtime API/KDMAPI/WinMM smoke tests;
- Ziggy and representative Black-MIDI listening/performance runs.

Audio reports should include duration, peak, RMS, correlation when alignment is
meaningful, spectral difference where useful, source/logical/region launch
counts, peak voices, retirements, steals/drops, event time, synthesis time, and
callback budget percentage.

## Baseline observed before migration edits

The repositories were already dirty when this inventory began. Existing edits
were preserved.

- V4 configured core tests: 3/3 passed (`midisynth_tests`,
  `midisynth_midi_tests`, `midisynth_wav_tests`).
- V3 configured tests: 14 passed, 1 failed, 1 skipped-equivalent within the
  15-test run. `svms_v3_correctness` reported 431 existing phase-rotation /
  Hilbert-pair failures; `svms_v3_asio_stress` was skipped. This is the
  pre-migration baseline and must not be hidden by canonical work.

No V3 scheduling, ingress, API, host, synthesis, or DSP source was modified in
Phase 1.

## Phase 2 implementation record

The canonical module now lives in `src/V3/Engine`. It is a V3-owned adaptation
of V4's `sample_bank`, `soundfont`, `channel_interpreter`, `synth`, and AVX2
kernel. `CanonicalEngine` is the host-agnostic raw-event facade; the imported
`Synth` remains the internal prepared-region core. Samples are held once as
raw signed 16-bit PCM and every scalar/AVX2 load uses the exact
`1.0f / 32768.0f` conversion. AVX2 remains a lane implementation over the
same authoritative SoA. Worker execution remains above the kernel and reduces
tiles in fixed numeric order.

The only host integration is `svms_v3_render --synth legacy|canonical`.
Realtime driver code, public APIs, KDMAPI, ingress, scheduler, cancellation,
reset fences, and callback pacing were not changed. Canonical mode never calls
a legacy voice manager, planner, scalar renderer, SSE2 renderer, AVX2 renderer,
dense path, sparse path, or whole-voice path. Capacity pressure uses a
deterministic quiet/release-biased canonical victim scan; ghost tails and exact
legacy Quality-heap scoring remain compatibility work.

### Concrete feature matrix

| Feature | Legacy V3 | Canonical engine | Offline canonical status | Action needed |
|---|---:|---:|---|---|
| Velocity `(velocity/127)^2` | Yes | Yes | Implemented/tested | None |
| Constant-power region pan | Yes | Yes | Implemented/tested | Add file-based pan fixtures |
| SF2 DAHDSR | Yes | Yes | Implemented/tested | Broaden real-SF2 corpus |
| Hold/decay key scaling | Yes | Yes | Implemented/tested | Broaden real-SF2 corpus |
| Pitch bend | Yes | Yes | Implemented | Add raw-facade exact-frame test |
| RPN 0,0 bend range | Yes | Yes | Implemented | Add raw-facade exact-frame test |
| CC7 / CC10 / CC11 | Yes | Yes | Implemented | Add file-based controller report |
| CC64 sustain | Yes | Yes | Implemented/tested | Add repeated-key file differential |
| CC120 all sound off | Yes | Yes | Implemented | Add file-based lifecycle test |
| CC121 reset controllers | Yes | Yes | Implemented | Add file-based lifecycle test |
| CC123 all notes off | Yes | Yes | Implemented | Add sustain interaction fixture |
| Program and bank changes | Yes | Yes | Implemented | Add V3 stack-route bridge |
| Layered presets | Yes | Yes | Implemented/tested | Add representative layered SF2 render |
| Sample loops | Yes | Yes | One-shot and continuous loops work | Add loop corpus |
| `sampleModes=3` release-loop exit | Yes | No | Explicit warning; continuous loop used | Implement release loop mask/state |
| Coarse/fine/scale tuning | Yes | Yes | Implemented | Add master tune/transpose |
| Root key override | Yes | Yes | Implemented | Add file-based fixture |
| SF2 low-pass filter | Yes | No | Unsupported, warned | Add ordinary-lane filter state/kernels |
| Default modulators | Partial/custom | No | Unsupported | Define intended set, implement once |
| Explicit modulators | Partial/parser | No | Unsupported, warned | Inventory and implement supported set |
| Vibrato/modulation LFO | Yes/partial | No | Unsupported; phase rotation mode rejected | Add canonical LFO state/kernels |
| Modulation envelope | Parsed only | No | Unsupported, warned | Implement without alternate renderer |
| `exclusiveClass` | Yes | No | Unsupported, warned | Add channel/class ownership index |
| Forced key/velocity | Parser-dependent | No | Unsupported, warned | Apply during preparation |
| Drums/percussion | Yes | Yes | Channel 10 resolves bank 128 then fallback | Add representative drum file report |
| SF2 stack/routes | Yes | No | Single offline path only | Bridge V3 immutable bundle/route policy |
| SFZ | Yes | No | Canonical load rejects it | Prepare supported SFZ into canonical data |
| 31-EDO | Yes | No | Explicitly rejected | Add host-prepared tuning term |
| Phase rotation/Hilbert | Dirty baseline | No | Explicitly rejected | Wait for semantics/baseline stabilization |
| Per-channel limiter buses | Optional | No | Stereo master limiter only | Add canonical bus render target |
| Deterministic capacity handling | Yes | Yes | Quiet/release-biased stealing | Match accepted Quality/ghost semantics |

### Phase 2 validation snapshot

- `svms_canonical_engine_tests`: passed. It covers exact-frame NoteOn,
  stable same-frame NoteOff/NoteOn order, prepared layering, velocity/pan,
  sustain logical ownership, scalar/AVX2 audio within `2e-5`, serial/threaded
  byte equality, and zero heap allocations in a warmed render call.
- Representative offline render: `X-Cool 3! 31edo F1.mid` was rendered in
  ordinary 12-EDO mode through the same 13,807 decoded events and `gm.sf2`.
  Canonical scalar and AVX2 each produced 4,778,606 frames. AVX2 versus scalar:
  max absolute sample difference `6.25849e-7`, difference RMS `5.88029e-8`,
  correlation `0.999999999999894`. Threaded AVX2 was byte-identical to serial
  AVX2 for this render.
- Legacy scalar produced 4,822,706 frames, peak 1.0 and RMS 0.272226; canonical
  scalar produced 4,778,606 frames, peak 1.0 and RMS 0.127642. Their aligned
  correlation was 0.00113 and difference RMS 0.301647. This is classified as
  a real semantic/coverage difference, not a tolerance issue: canonical still
  lacks V3 filters, modulation, release-loop exit, stack routing, tuning mode,
  and other rows above. It is not ready to replace realtime synthesis.
- Performance baseline (2048 frames, M voice-samples/s): scalar serial
  64/1k/10k = 261/262/260; scalar 3-worker = 258/730/726; AVX2 serial =
  210/234/207; AVX2 3-worker = 214/695/892. At 10k voices with an event on
  every frame: scalar serial/threaded = 252/520; AVX2 serial/threaded =
  229/606. The scalar sample loads are the oracle; the current AVX2 int16
  lane gather is deliberately simple and is a measured future optimization.
- `build_v3.bat`: passed after closing the configurator that held `winmm.dll`;
  the configurator was restarted afterward.
- Full V3 CTest: 14 passed, 1 failed, 1 skipped in the 16-test run. The only
  failure remains `svms_v3_correctness` with exactly 431 pre-existing
  phase-rotation/Hilbert failures. The canonical test is the added passing
  test; no legacy result changed.
- XP x86 configuration still excludes the phase-2 module and makes no source
  dependency on it. The XP build was attempted at `-j4` and `-j1`; both stopped
  in unchanged `SVMSDriver.cpp`/`SVMSStandaloneSynth.h` with MSVC C1060
  (compiler out of heap space), before link or tests. No canonical file was in
  either failing compile command.

The offline CLI historically applies `LimiterRouterState` but not the driver's
embedded `ReverbState`. Phase 2 preserves that same post chain for both
selectors instead of silently inventing a second reverb. Mechanical extraction
and impulse/stream parity for the existing V3 reverb therefore remains an
explicit offline compatibility item.

Cohort compression, AVX512, GPU synthesis, and realtime integration were not
implemented.

## Phase 3 synthesis-coverage investigation and priority (2026-09-18)

The first instrumented A/B render used the same 13,807 source events,
`gm.sf2`, 44.1 kHz rate, scalar backend, limiter settings, block size, and
one-second maximum-tail setting on both paths. The source stream contained
2,474 NoteOns, 2,474 NoteOffs, 7 program changes, 3,957 control changes, and
4,895 pitch bends. It contained no CC64/120/121/123 events.

The 44,100-frame output-length difference is an end-condition difference,
not an unexplained DSP offset and not a fixed pad in the writer. At the MIDI
end frame legacy still reported 15 active voices (2 remained after the capped
tail) and therefore rendered the complete configured 44,100-frame tail.
Canonical reported zero active voices and correctly rendered no tail. The host
loop is `while (active-or-tail && frame < tailEnd)`; adding an unconditional
second would conceal the lifecycle difference and is explicitly not the fix.
The initial counters also exposed an earlier comparison issue: only 948
canonical physical launches were observed versus 2,474 legacy launches, while
3,052 canonical source events were rejected. The rejection breakdown was
`invalid=3052, order=0, capacity=0, interpreter=0`: the fixture contains
exactly 1,526 pairs of SVMS extended-key NoteOn/NoteOff events whose key byte is
128..255. Legacy accepts that project extension; the canonical 7-bit MIDI
boundary intentionally rejects it because 31-EDO/extended keys are outside
this phase. Consequently this particular file remains useful as an explicit
unsupported-extension/Black-MIDI check, but its legacy/canonical RMS and
correlation are not evidence about ordinary SF2 synthesis parity.

### Synthesis-feature priority matrix

| Feature | Legacy V3 behavior | Canonical behavior at phase start | Audible importance | Lifecycle importance | Priority | Test coverage required |
|---|---|---|---|---|---:|---|
| Offline boundary acceptance / region accounting | Dispatches the prepared stream immediately and reports match failures | Instrumented run rejected 3,052 events and launched 948 regions | Critical prerequisite | Critical prerequisite | 0 | Exact event counts, first/last nonzero, match/launch/retire/drop breakdown |
| SF2 low-pass filter | Per-voice resonant two-pole DF2T from `initialFilterFc/Q` | Recognized but warned/ignored | Very high | Low | 1 | Open, low/high cutoff, resonance, scalar/AVX2/thread |
| `exclusiveClass` | Channel-local choke; all layers in the victim logical note enter the fixed choke release | Recognized but warned/ignored | High for percussion | Very high | 2 | Layered groups, channels, sustain, same-frame order |
| `sampleModes=3` release-loop exit | Loops while held, disables looping when release begins | Treated as continuous loop | High for affected patches | Very high | 3 | NoteOff, sustain-up, choke, forced release, exact loop boundary |
| Forced key / velocity | Incoming ownership key remains MIDI key; effective synthesis key/velocity override region synthesis inputs | Recognized but warned/ignored | Medium | Medium | 4 | Match/ownership distinction, pitch, gain and key-scaled envelopes |
| SF2 default modulators | V3 has explicit controller behavior plus a partial/custom SF2 set | Only explicit engine controller semantics; velocity attenuation deliberately replaced by square curve | High | Medium | 5 | Each retained default independently; double-application guards |
| Explicit `pmod` / `imod` | Parser data exists; effective subset requires audit | Chunks validated then warned/ignored | High on authored fonts | Medium | 6 | Source transform, amount source, destination composition, unsupported report |
| Vibrato/modulation LFO | Voice vibrato support and partial modulation behavior | No canonical LFO | High on synth/brass/pad | Low | 7 | Delay/frequency/phase and pitch/filter/volume destinations |
| Modulation envelope | Generators are parsed in legacy paths but effective support is incomplete | Recognized but warned/ignored | Medium-high on synth/pad | Medium | 8 | Full DAHDSR, key scaling, pitch/filter destinations, release |

Release-loop exit is moved ahead of modulators because it directly controls
voice lifetime and is a plausible contributor to the independently observed
tail mismatch. Forced key/velocity is also moved earlier because it is a
preparation/ownership correctness issue with a small, well-bounded runtime
surface. Filters remain first among DSP features because they are ubiquitous
and dominate ordinary-SF2 timbre. This ordering does not authorize a legacy
fallback or a renderer-specific feature path.

## Phase 3 implementation and validation record (2026-09-18)

Phase 3 keeps the Phase 2 engine boundary unchanged. The V3 offline producer
still supplies its stable, exact-frame event stream to `CanonicalEngine`; the
canonical SoundFont preparation, channel interpreter, logical-note layer and
SoA own every feature below. V3 still applies the existing offline high-pass /
limiter path and writes the WAV. No scheduler, ingress, realtime callback,
KDMAPI, public ABI, or audio-host source was changed.

### Implemented synthesis semantics

- **Low-pass filter.** Canonical voices now own the two-pole DF2T coefficient
  and history rows. `initialFilterFc` (absolute cents) and `initialFilterQ`
  (centibels) are prepared at load/note expansion; velocity, modulation-LFO,
  modulation-envelope, and supported explicit-modulator cutoff terms compose
  before coefficient evaluation. An effectively open cutoff carries no
  recursive state. Scalar and AVX2 update the same per-voice history and worker
  tiles do not own filter state.
- **`exclusiveClass`.** A NoteOn gathers the classes of all matching regions.
  Before its layers launch, prior voices with a matching class on the same MIDI
  channel cause every layer of the prior logical note instance to enter a
  deterministic 10 ms release. The choke overrides sustain, does not cross
  channels, and follows exact same-frame dispatch order. It is logical-note
  lifecycle behavior, not a render-kernel special case.
- **Release loops.** `sampleModes=3` loops while held and clears the loop flag
  when canonical release begins. This covers direct NoteOff, deferred sustain
  release, exclusive-class choke, and the common forced-release path. CC120 /
  reset still retire immediately rather than manufacturing a release tail.
- **Forced key and velocity.** Region matching and NoteOff ownership retain the
  incoming MIDI key. The forced values affect synthesis pitch, key-scaled
  envelope timing, the square velocity gain, the implicit velocity-to-filter
  term, and static key/velocity explicit modulators.
- **LFOs.** Each voice owns deterministic triangle modulation- and vibrato-LFO
  phase, delay, and frequency state. Supported destinations are pitch, filter
  cutoff, and volume. CC1 controls the default vibrato depth through canonical
  channel state and CC121 resets it. Scalar and AVX2 advance the same state one
  sample at a time.
- **Modulation envelope.** A canonical per-voice delay/attack/hold/decay/
  sustain/release state machine supplies pitch and filter destinations.
  Hold/decay key scaling and release entry share the canonical lifecycle.
- **Explicit `pmod` / `imod`.** Global/local replacement is resolved at load;
  preset modulators then add to instrument modulators. Immutable numeric rows
  are evaluated at note preparation. The supported subset accepts constant,
  MIDI-key, and MIDI-velocity linear sources, linear or absolute output
  transforms, and destinations for tuning/pitch, pan, attenuation, filter,
  both LFOs, the modulation envelope, and the volume envelope. Recognized rows
  outside this subset are listed with their numeric source, destination,
  amount-source, and transform in SoundFont warnings; they are not silently
  discarded or routed to legacy synthesis.

### SF2 default-modulator disposition

The standard defaults are represented exactly once as follows:

| Default | Canonical disposition |
|---|---|
| Velocity to attenuation | Intentionally replaced by the V3/SVMS compatibility law `(velocity/127)^2`; not stacked |
| Velocity to filter cutoff | Implemented, default -2400-cent low-velocity term; an identical instrument modulator replaces its amount |
| Channel pressure to vibrato | Unsupported: channel pressure is not yet in the canonical boundary vocabulary |
| CC1 modulation wheel to vibrato | Implemented, 50-cent default depth; an identical instrument modulator replaces its amount |
| CC7 volume to attenuation | Existing canonical CC7 gain semantics, not stacked |
| CC10 pan | Existing canonical constant-power channel pan semantics, not stacked |
| CC11 expression to attenuation | Existing canonical CC11 gain semantics, not stacked |
| CC91 reverb send | Not applicable yet: the offline canonical target exposes one stereo mix and no per-voice send bus |
| CC93 chorus send | Not applicable yet: there is no canonical chorus/send bus |
| Pitch wheel times pitch-wheel sensitivity | Existing canonical pitch bend plus RPN 0,0 range, not stacked |

### Updated feature-priority matrix

| Feature | Legacy V3 behavior | Canonical behavior after Phase 3 | Audible importance | Lifecycle importance | Result / next priority | Test coverage |
|---|---|---|---|---|---|---|
| SF2 low-pass filter | Per-voice resonant two-pole filter | Same canonical DF2T state in scalar/AVX2 | Very high | Low | Implemented; broaden real-font corpus | Open/low cutoff, coefficient resonance, modulation differential |
| `exclusiveClass` | Channel-local group choke | Channel-local complete logical-note group release | High | Very high | Implemented | Layer group, channel isolation, deterministic order |
| `sampleModes=3` | Exit loop on release | Loop flag clears on canonical release | High | Very high | Implemented | NoteOff/release boundary fixture |
| Forced key / velocity | Effective synthesis overrides, incoming ownership key | Same separation | Medium | Medium | Implemented | Pitch/gain/key ownership assertions |
| SF2 default modulators | Explicit controllers plus partial/custom SF2 behavior | Applicable defaults implemented once; exceptions above | High | Medium | Partial by design | Velocity/filter and CC1 exercised with DSP differential |
| Explicit `pmod` / `imod` | Partial legacy interpretation | Prepared static key/velocity subset; unsupported tuples warned | High | Medium | Partial; dynamic sources next | Static velocity-to-pan preparation plus parser warnings |
| Modulation LFO | Voice modulation behavior | Per-voice triangle LFO to pitch/filter/volume | High | Low | Implemented | Delays/steps/destinations in scalar/AVX/thread differential |
| Vibrato LFO / CC1 | Voice vibrato and modulation wheel | Per-voice vibrato with channel CC1 depth | High | Low | Implemented | Channel event plus scalar/AVX/thread differential |
| Modulation envelope | Partial legacy generator coverage | Canonical DAHDSR to pitch/filter | Medium-high | Medium | Implemented | All prepared stages, key scaling paths, release and DSP differential |
| Ordinary program/bank/drums | Supported | Supported, including bank-128 channel 10 lookup/fallback | High | Medium | Retained | Ordinary-GM multi-program fixture |

Key-to-filter tracking is available through the supported static MIDI-key
explicit-modulator source; SF2 has no additional implicit key-to-cutoff default.
Dynamic explicit CC, channel-pressure, pitch-wheel, poly-pressure, linked
modulators, and concave/convex/switch explicit-source transforms remain
ordinary-SF2 gaps. The two implicit non-linear defaults are handled directly
as described above rather than passed through that restricted explicit-source
evaluator.

### Differential investigation and audio results

The original extended-key Black-MIDI fixture remains intentionally outside
ordinary MIDI coverage. Its 13,807-event stream contains 1,526 NoteOns and
1,526 matching NoteOffs with key bytes 128..255. Canonical rejected exactly
those 3,052 events as invalid 7-bit MIDI (`order=0`, `capacity=0`,
`interpreter=0`) and launched the remaining 948 voices; legacy accepted all
2,474 NoteOns as the SVMS 31-EDO/extended-key extension. After this phase:

- canonical scalar and AVX2 each rendered 4,778,606 frames;
- scalar versus AVX2 max difference was `5.96046448e-7`, difference RMS
  `5.74821717e-8`, correlation `0.9999999999998768`;
- legacy still rendered 4,822,706 frames because its 15 live voices at the
  MIDI end triggered the complete 44,100-frame tail cap, while canonical had
  no live voice and emitted no tail;
- legacy peak/RMS were `1.0 / 0.272226`; canonical peak/RMS were
  `1.0 / 0.12769370`. This is primarily an excluded extended-key comparison,
  so its whole-song correlation is not used as an ordinary-SF2 quality claim.

An ordinary 16-second GM fixture was therefore added as the attributable A/B
workload: piano, pad, synth brass, channel-10 percussion, sustain, CC1/7/10/11,
pitch bend, and open/closed hats. Both paths consumed 384 events and 160
NoteOns and produced 793,800 frames, including the same two-second configured
tail cap. Canonical accepted every source event and launched 160 physical
voices with zero rejection. Results after the shared offline post chain:

| Measurement | Legacy | Canonical scalar | Canonical AVX2 |
|---|---:|---:|---:|
| First nonzero frame | 177 | 220 | 220 |
| Last nonzero frame | 793,799 | 793,799 | 793,799 |
| Peak | 0.9032374 | 1.0 | 1.0 |
| RMS | 0.15742658 | 0.16485530 | 0.16485530 |
| Spectral centroid | 589.904 Hz | 577.583 Hz | 577.583 Hz |
| Peak voices | 27 | 24 | 24 |
| Active at MIDI end | 20 | 17 | 17 |
| Retired / active after capped tail | 152 / 8 | 155 / 5 | 155 / 5 |

Canonical scalar versus AVX2 measured max `5.51342964e-7`, difference RMS
`9.05055219e-8`, correlation `0.999999999999843`; serial versus three-worker
AVX2 output was byte-identical. Legacy versus canonical aligned correlation
was `0.81681157`, with difference RMS `0.09779376`. This is substantial
convergence from the misleading extended-key comparison, but not parity: the
43-frame onset difference and remaining gain/lifecycle differences are still
visible and must be attributed before realtime replacement.

Large real Black-MIDI candidates with 124-209 million decoded events were
scan-checked but not claimed as completed audible renders; a full render would
not be a useful ordinary-SF2 gate for this phase. The ordinary-GM fixture and
the original dense extended-key render are the completed real-path checks.

### Correctness, allocation, threading, and performance

`svms_canonical_engine_tests` passes. It covers exact-frame ordering,
ownership/sustain, filter preparation/open behavior, exclusive-class layered
choke, forced key/velocity, release-loop exit, LFO/mod-envelope state, a static
explicit modulator, dense scalar/AVX2 rendering, scalar and AVX2 worker
differentials, and allocation counting. The feature-dense synthetic run was
byte-identical between its scalar and AVX2 results; the file-based comparison
above supplies the nonzero differential measurement. Both scalar and AVX2
were byte-identical between serial and three-worker execution. A warmed
feature-dense render performed zero heap allocations.

Performance in M voice-samples/s (same 2048-frame benchmark matrix) is:

| Backend / workers | 64 | 1k | 10k | dense 10k | Phase 2 baseline (64 / 1k / 10k / dense) |
|---|---:|---:|---:|---:|---|
| Scalar / 0 | 219.193 | 205.138 | 214.402 | 202.794 | 261 / 262 / 260 / 252 |
| Scalar / 3 | 214.384 | 609.819 | 586.721 | 570.397 | 258 / 730 / 726 / 520 |
| AVX2 / 0 | 273.424 | 270.029 | 268.619 | 258.454 | 210 / 234 / 207 / 229 |
| AVX2 / 3 | 257.851 | 868.217 | 959.300 | 673.782 | 214 / 695 / 892 / 606 |

The scalar rows regress roughly 16-22% on the non-dense cases after expanding
the authoritative SoA and lifecycle/modulation checks; threaded dense scalar
and every measured AVX2 row improved on this run. These are baseline results,
not justification for a specialized feature renderer. The benchmark's seeded
voices do not enable every newly added LFO/filter destination, so it should not
be read as a worst-case all-features cost.

`build_v3.bat` passed. Full V3 CTest is unchanged at 14 passed, 1 failed, and
1 skipped: the sole failure is still `svms_v3_correctness` with exactly 431
pre-existing phase-rotation/Hilbert-pair failures; `svms_v3_asio_stress` is the
skip. That subsystem and its count were not modified. The XP x86 retry again
stopped while compiling unchanged legacy `SVMSDriver.cpp` through
`SVMSStandaloneSynth.h` with MSVC C1060 (compiler out of heap space), before
any canonical source or link/test step; the canonical target remains excluded
from `SVMS_XP_COMPAT`.

### Remaining coverage

Ordinary-SF2 work still includes the dynamic explicit-modulator sources and
source curves listed above, channel pressure, effect-send buses, stereo-linked
sample behavior/corpus validation, and closer matching of legacy Quality
stealing/ghost-tail behavior. SVMS-specific work remains SFZ, 31-EDO/extended
keys, phase rotation, stacks/routes, master-tune extensions, and optional
per-channel limiter buses.

No canonical feature calls a legacy renderer or voice manager. Realtime V3
still uses legacy synthesis. Cohort compression was not implemented: one
logical layer remains one physical canonical voice. AVX512 and GPU work were
not added.

## Follow-up offline parity audit (2026-09-22)

The ordinary-GM A/B exposed a specific preparation mismatch: V3 pins SF2
volume-envelope timecents below `-11950` to zero, while canonical previously
interpreted the default `-12000` as about 43 frames at 44.1 kHz. In a
limiter-free render of the same 384-event fixture, legacy's first nonzero frame
was 1 and canonical's was 44. Canonical volume-envelope preparation now uses
the V3 near-zero pin and V3's 10 ms minimum release. LFO and modulation-
envelope timecents retain their separate SF2 timing. After this change both
paths first produce a nonzero sample at frame 1. This is the source of the
previously reported 43-frame onset gap, not an event-ordering error.

The end-of-song `releaseAll` operation now releases every canonical logical
note even if its channel sustain pedal is down, matching V3's unconditional
`VoiceManager::ReleaseChannel` behavior. The interpreter pre-counts all needed
release events against existing scratch capacity before mutating ownership.
The ordinary-GM fixture happened to have no sustain-held note at its end, so
its audio and final voice counts did not change from this lifecycle correction.
The targeted test covers that case.

With the same fixture, SoundFont, rate, volume, and **limiter disabled** on both
paths, legacy-versus-canonical correlation moved from `0.81656684` to
`0.81891073`; difference RMS moved from `0.09794013` to `0.09751477`, and
maximum sample difference from `0.76647824` to `0.59418949`. Both still
produce 793,800 frames. Legacy has 20 active voices at MIDI end and 8 after
the capped tail; canonical has 17 and 5. The matching onset solves one
identified difference, while the remaining audio and lifecycle delta is
substantial and needs per-preset/region attribution before realtime selection.

The updated ordinary-GM render measured canonical scalar versus AVX2 maximum
`5.364418e-7`, difference RMS `8.631918e-8`, correlation
`0.9999999999998721`; AVX2 serial versus three-worker WAVs were byte-identical.
The canonical scalar/AVX2/thread/allocation test passed. In this run, offline
DSP time was about 10.8 ms per second of audio for canonical scalar and 2.3 ms
for legacy scalar; these are observed fixture costs, not controlled benchmark
throughput. Dynamic per-sample filter/modulation work is the leading profiling
candidate. The Phase 3 benchmark matrix above did not enable every new
destination, so its higher throughput is not representative of this fixture.

The follow-up `build_v3.bat` passed. Full CTest listed 14 passed, one failed,
and one skipped among 16 registered tests. Its `94%` summary counts the skip
as non-failing; earlier records calling this "15 passed, 1 failed, 1 skipped"
counted the skip twice. The sole failure still reports exactly 431
pre-existing Hilbert/phase-rotation assertions.

The next ordinary-SF2 work should be isolated program/region audio
comparisons, especially envelope release/retirement, filter modulation, and
effective gain. After those semantics are understood, profile the canonical
per-sample modulation/filter update and implement remaining dynamic explicit
modulators. Channel pressure and effect sends require a deliberate extension
of the canonical event/output boundary. Quality stealing and ghost tails need
capacity-pressure tests. Realtime integration remains a later decision.

## Stop conditions

Stop and report rather than proceeding if any implementation requires:

- replacing V3 queue/compiler/scheduler/timestamp logic;
- importing V4 realtime/API/WASAPI infrastructure;
- allocating, locking, logging, loading files, or parsing configuration in the
  audio callback;
- selecting legacy synthesis for a feature inside canonical mode;
- maintaining mirrored scalar/SIMD voice state;
- changing same-frame ordering, exact offsets, reset fences, cancellation, or
  callback pacing;
- loosening a differential tolerance to obtain a pass.

The success criterion remains: V3 is the mature host, and the canonical module
is the only synth.
