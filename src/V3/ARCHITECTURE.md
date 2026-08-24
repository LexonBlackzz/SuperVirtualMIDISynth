# SuperVirtualMIDISynth V3 Architecture

## Contract

V3 is a drop-in Windows `winmm.dll` synthesizer and a reusable native MIDI
engine. Live WinMM, KDMAPI-compatible, SVMS API, Linux ALSA, and offline paths
share the same scheduler, MIDI state, SoundFont launch, stealing, and synthesis
semantics.

The non-negotiable timing contract is:

- every admitted event is assigned an absolute integer output frame;
- equal-frame events execute in global ingress-sequence order;
- controller changes split rendering at their exact frame;
- rendering work may be chunked or concurrent, but events are never quantized
  to callback, worker, tile, or 128-frame planning boundaries; and
- priority overload may shed documented note-ons, while lossless overload
  retains every event and reports lateness if real time cannot be maintained.

The selectable logical voice ceiling is 524,288. That is a storage ceiling,
not a promise that every configuration can render 524,288 full-quality voices
in real time.

## Runtime Pipeline

```text
WinMM / KDMAPI facade / SVMS API / ALSA / offline MIDI reader
                         |
                         v
       runtime-sized sequence-numbered MPSC priority lanes
        state | loud | upper-medium | medium | quiet
                         |
                         v
                one compiler worker
       QPC -> absolute frame, stable 8,192-event pages
                         |
                         v
         page-head winner-tree scheduled backlog
                         |
                         v
                   audio callback
       extract bounded due set, preserve (frame, sequence)
                         |
                         v
            exact-frame MIDI/voice dispatch
                         |
            +------------+-------------+
            |                          |
     event-free spans          128-frame dense plan
     class kernels and         immutable mutations and
     profitable workers        deterministic voice tiles
            |                          |
            +------------+-------------+
                         v
     planar mix -> reverb -> lookahead limiter/high-pass
                         |
             live recorder tap -> audio backend
```

## Ingress and Scheduling

`PriorityEventIngress` contains five preallocated MPSC lanes sized from
`events.ring_capacity`. State events and velocity 96-127 note-ons use
cancellable backpressure. Lower-velocity lanes may be shed progressively in
priority mode. Note-offs, controller/state events, panic, reset, and engine
commands are never intentionally discarded.

Producers receive a QPC timestamp and global sequence before waiting. The
compiler worker drains the lane heads in sequence order, converts QPC time to
absolute frames with remainder-preserving integer arithmetic, and fills
immutable pages of 8,192 `ScheduledRenderEvent` records. A page is stably
ordered by `(targetFrame, ingressSequence)` using merge/radix paths selected by
its run shape.

The audio thread imports page descriptors into a fixed page-head winner tree.
Advancing the next scheduled event is logarithmic in the number of live pages;
event payloads are not merged, compacted, or recopied as backlog grows.
Exhausted pages return to the compiler through a lock-free free-page queue.

Only events due before the callback end are copied into the preallocated render
event buffer. `events.max_events_per_block` bounds callback work independently
of queue capacity. Excess due work remains ordered and is explicitly late; it
is never rescaled onto a callback grid.

Priority mode may coalesce adjacent equal-frame writes only when the earlier
write is provably stateless and completely overwritten by the later write.
Lossless mode disables intentional coalescing and shedding.

## MIDI and SoundFont State

The audio thread is the sole owner of channel state, note generations, voice
allocation, and victim decisions. Per-channel active indices and per-channel/
key generation queues make controller changes, bends, sustain/sostenuto, note
offs, CC120, CC121, and CC123 local rather than full-pool scans.

SoundFonts load and resample off-thread into immutable bundles. The audio
thread activates a completed bundle only at a block boundary; retired bundles
are reclaimed on the control thread. A priority-ordered SoundFont stack and
bank/preset routes are flattened into one render sample store. Cached launch
plans resolve the common eight-or-fewer-layer case by SoundFont generation,
preset, channel pitch revision, note, and velocity.

Common XG reset, master tuning/volume, and multipart messages are translated to
ordinary exact-frame engine commands. Unsupported SysEx remains observable but
does not perturb scheduling.

## Voice Pool and Exact Stealing

`VoiceManager` owns a capacity-sized `VoiceSoA`, lifecycle indices, render-class
lists, channel indices, and note-generation queues. Allocation pops a free
slot; retirement uses inverse active positions and swap removal. `activeList`
exists for lifecycle and exact tie semantics, not render traversal.

Every MIDI note-on launches a fresh atomic play group. Mono notes occupy one
physical voice; stereo or layered regions occupy multiple voices sharing one
`playIndex`. Allocation and stealing replace the complete group so one stereo
side cannot disappear independently.

Stable steal candidates live in a persistent winner tree keyed by the current
BASS-like score and original active position. Attack/decay/release candidates
whose scores vary with render progress live in a per-frame exact heap. The two
winners are compared with the same tie rule as exhaustive selection. Matching
groups can be rewritten in place while preserving active and channel slots.

Audible victims may enter a fixed 50-entry, 64-frame outgoing-tail reserve.
The reserve replaces its quietest existing tail when necessary; it does not
allocate one tail record per configured voice.

Primary voice arrays share a 64-byte-aligned allocation sized to configured
polyphony. Write-only legacy fields have been removed. The dense multicore
shadow reserves only audible/render state; cold MIDI identity, linkage, pitch,
and stealing metadata remains solely in the authoritative pool.

## Rendering

`RenderScalar::RenderBlock` is the shared live/offline entry point. Production
rendering splits sparse blocks into exact event-free spans, dispatches the
equal-frame event run at each boundary, and renders persistent class lists:

- sustained looping;
- sustained one-shot;
- looping attack/decay;
- looping release;
- one-shot envelope/release;
- rare generic voices; and
- independent steal tails.

Scalar, SSE2, and AVX2 kernel sets share the same span interface. Backend
selection happens once at initialization after CPUID/OSXSAVE/XCR0 validation.
XP contains scalar/SSE2 paths only. Kernels preserve linear interpolation,
multi-loop overshoot, exact envelope/release countdowns, phase state, and
retirement frames.

The old frame-major renderer is compiled only into reference builds and serves
as the differential oracle.

## Multicore Rendering

`synth.render_threads = 1` selects serial rendering, an explicit larger value
selects that total thread count, and `0` selects a topology-aware automatic
count. Persistent workers use MMCSS and FTZ/DAZ. Modern Windows uses generation
counters with dynamically resolved `WaitOnAddress`; XP retains event waits.

Long profitable spans become dynamically claimed logical voice-tile jobs.
Each tile writes a private stereo scratch slice, and the audio thread reduces
tiles in fixed logical order, so worker execution order cannot change output.

Dense event blocks use a separate exact 128-frame planner for up to 8,192
configured voices. The audio thread walks all events in exact order and records
per-voice mutation snapshots at their true offsets. Workers render immutable
256-handle tiles while the coordinator prepares the next chunk. The 128-frame
unit packages work only; it does not delay events. Capacity checks use the
actual union of affected voice populations. If a plan cannot be represented,
that chunk falls back to the exact serial path without truncation or delay.

## Audio and DSP

Modern Windows uses event-driven WASAPI shared mode with device-sized aligned
buffers and automatic recovery after endpoint invalidation, service restart,
or default-device change. Recovery retries the configured endpoint and then
the current default endpoint without resetting MIDI/voice state. XP uses the
DirectSound/waveOut compatibility path.

Post processing consists of optional reverb, a configurable lookahead limiter,
and a stateful 3 Hz DC/subsonic high-pass. Live parameter changes arrive through
an atomic mailbox and glide where coefficient steps would click. Live WAV/RF64
recording copies the post-DSP stereo stream into a bounded SPSC buffer and
writes it on a background thread.

The callback never allocates, takes a general-purpose lock, displays UI, or
emits debug output. Diagnostics are published through atomic snapshots and the
versioned runtime link.

## Configuration and Compatibility

V3 reads a DLL-local `config.json` first, then
`%APPDATA%\\SuperVirtualMIDISynth\\config.json`, then environment overrides.
The first writable configuration is created atomically and can import a legacy
`config.ini` without modifying it. Relative SoundFont paths resolve against the
driver directory. Unknown JSON fields survive saves; malformed or unsupported
newer schemas are never overwritten.

The Configurator is optional. Runtime-link structures are versioned,
capability-gated, and fixed-size where published. Build-number mismatch is a
user notification, not a protocol decision. The native SVMS API uses
size/version negotiation, and the KDMAPI facade remains a compatibility layer.

## Current Scaling Boundary

The scheduler is no longer the primary Black MIDI bottleneck. At saturated
polyphony, exact note launch, grouped victim replacement, tail capture,
lifecycle maintenance, and synthesis dominate. The 524,288 ceiling is usable
for storage experiments, but predictable 500K operation still requires paged
SoA storage, compact inactive/tail representations, and an explicit memory
budget. Those items remain tracked in `ROADMAP.md`.

## File Map

| File | Responsibility |
|---|---|
| `SVMSDriver.cpp` | WinMM/native entry points, compiler worker, exact MIDI dispatch, bundle activation, audio callback and DSP |
| `SVMSEventPages.h` | compiled page pool, stable page ordering, page-head winner tree |
| `SVMSEventScheduler.h` | reference and small-batch scheduler machinery |
| `SVMSMPSCQueue.h` | runtime-sized priority ingress lanes and cancellable waits |
| `SVMSVoiceManager.h` | voice lifecycle, channel/key indices, launch transactions, exact grouped stealing |
| `SVMSTypes.h` | shared POD types and primary/tail SoA storage |
| `SVMSRenderScalar.h` | exact span dispatcher, dense planner, oracle boundary |
| `SVMSRenderKernels*.cpp` | scalar, SSE2, and AVX2 class kernels |
| `SVMSRenderWorkers.*` | persistent deterministic tile workers |
| `SVMSSoundFont.*` | SF2 parsing, region compilation, validation, and resampling |
| `SVMSAudioOutput.h` | WASAPI recovery and XP output compatibility |
| `SVMSConfig.*` | JSON migration, validation, precedence, and atomic persistence |
| `SVMSRuntimeLink*` | versioned diagnostics/control protocol |

See `ROADMAP.md` for accepted measurements, rejected directions, and the
remaining path toward efficient 500K-voice operation.
