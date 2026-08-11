# SuperVirtualMIDISynth V3 Roadmap

The end goal is a synthesizer that remains musically useful with roughly
500,000 simultaneous voices and very dense input bursts reaching tens of
millions of note events per second on a modern CPU. The scalar renderer remains
the compatibility and correctness baseline, including on very low-end CPUs.

A checked item is implemented in the current V3 tree. It does not imply that a
larger phase containing that item is complete.

## Current Baseline: Functional 4K Scalar Engine

- [x] CMake/Ninja build, `build_v3.bat`, and release-build verification
- [x] SF2 parser, preset/instrument region compilation, and resampled storage
- [x] Preset/instrument global and local zone merging with layered key/velocity
  matching
- [x] Index compiled SF2 regions by preset so dense note-ons never scan regions
  belonging to unrelated banks and programs
- [x] WASAPI shared-mode output and `winmm.dll`/KDMAPI-compatible entry points
- [x] SoA voice state with flat active list, inverse active positions, and free
  stack
- [x] Fresh allocation for overlapping same-key retriggers
- [x] BASSMIDI-like voice stealing based on effective control/envelope level
  and rendered age rather than raw velocity, with at most 50 independent
  64-frame outgoing tails and an unblurred replacement attack
- [x] Scalar fused per-sample renderer with linear interpolation
- [x] SF2 volume envelopes, sample loops, attenuation, tuning, and release tails
- [x] Configured velocity threshold/curve/floor, velocity LUT,
  CC7/CC10/CC11 gains, and precomputed loop bounds
- [x] Full-quality rendering throughout the current 4096-voice hard pool;
  adaptive decimation is reserved for future larger storage
- [x] Limiter, double-buffered diagnostic statistics, and optional diagnostic
  window

### Compatibility Builds

- [x] Provide a separate modern Windows x86/WASAPI build through
  `build_v3_x86.bat`
- [x] Provide a separate XP-compatible x86/SSE2 build through
  `build_v3_xp_x86.bat`, using the bundled MSVCRT-based MinGW toolchain
- [x] Replace WASAPI/MMCSS with a notification-driven DirectSound PCM output
  layer only in the XP target while retaining the common scalar renderer and
  sample-accurate scheduler
- [x] Use V1-compatible `DirectSoundCreate` interfaces on XP and fall back to
  ordered play-cursor feeding when legacy drivers reject position notifications
- [x] Fall back to event-driven `waveOut` through the absolute system
  `winmm.dll` when DirectSound cannot initialize, with forced backend and full
  DLL integration tests
- [x] Match V1's absolute-System32 WinMM loading and forward XP waveOut and
  mixer exports so DirectSound and legacy drivers see the real default device
- [x] Treat XP `buffer_frames` as the per-callback segment size (with a
  four-segment DirectSound safety ring) and show audio/SoundFont health in the
  XP diagnostic window, enabled by default for new XP configurations
- [x] Use XP's `SHGetFolderPathW` configuration path and avoid importing UCRT,
  API-set, Known Folder, WaitOnAddress, or other post-XP entry points
- [x] Export the complete undecorated WinMM/KDMAPI surface from the MinGW x86
  DLL and exercise DirectSound notification/start/stop behavior in smoke tests
- [x] Export Ziggy/SSV2-compatible active/free/steal statistics plus V2 render
  time/voice-count telemetry, retain the full diagnostic window, and emit a
  `SnappySynth.dll` direct-loader alias beside every `winmm.dll` build
- [x] Accept both device `0` and the legacy `MIDI_MAPPER` ID in WinMM open/caps
  calls, reject false-success opens when the backend cannot start, and cover
  DLL load/open plus diagnostic-window creation with end-to-end XP tests
- [ ] Run live MIDI/SF2 playback and shutdown stress on physical Windows XP
  hardware; PE/import auditing and 32-bit tests pass on the development host

## Stabilization Tranche

### JSON Configuration

- [x] Read portable `config.json` beside `winmm.dll` first, then fall back to
  `%APPDATA%\SuperVirtualMIDISynth\config.json`; create locally when AppData is
  unavailable
- [x] Vendor `nlohmann/json` and confine its use to `SVMSConfig.cpp`
- [x] Resolve Roaming AppData with `SHGetKnownFolderPath` and Unicode paths
- [x] Create the directory and complete schema-versioned defaults on first run
- [x] Serialize first-run/migration output through a PID-specific temporary file
  and atomic replace
- [x] Serialize concurrent creation with a cross-process named mutex
- [x] Apply compiled defaults, JSON, then environment overrides
- [x] Support `SVMS_NO_DROP_EVENTS`, diagnostics, diagnostic-window/debug-output,
  and correctness-mode environment overrides
- [x] Select a named WASAPI render endpoint through `audio.device` or
  `SVMS_AUDIO_DEVICE`; a missing configured endpoint fails safely instead of
  falling back to an unintended default output
- [x] Honor explicit absolute or DLL-relative SoundFont paths; when absent or
  missing, deterministically discover DLL-local `.sf2` files and record the
  discovered filename in newly created JSON without assuming `gm.sf2`
- [x] Import recognized `config.ini` values once from beside the DLL or host
  executable without modifying the INI
- [x] Preserve recognized legacy effects/limiter values in the migration JSON
  for a future schema
- [x] Reject invalid fields individually and publish a configuration warning
- [x] Leave malformed or newer-schema JSON untouched and run with defaults
- [x] Test first-run creation, concurrent creation, portable/AppData
  precedence and fallback, Unicode paths, INI import, invalid fields,
  malformed/newer schemas, and environment precedence
- [ ] Add a settings-save/update API that round-trips unknown fields; V3
  currently only writes during first creation/migration
- [ ] Add live configuration reload; configuration changes currently require an
  engine restart

### Audio Thread and WASAPI

- [x] Use event-driven WASAPI shared mode
- [x] Size render buffers from the actual device buffer
- [x] Run the audio thread under MMCSS `Pro Audio` with FTZ/DAZ enabled
- [x] Use aligned planar render buffers and preallocated callback storage
- [x] Cancel blocked MIDI producers during shutdown and join the audio thread
  before releasing WASAPI/engine state
- [x] Publish diagnostic state through a lock-free double buffer and run the
  diagnostic window/debug display on its own thread
- [ ] Extract a distinct audio-thread-owned `SynthCore` shared by live WASAPI
  and offline rendering
- [ ] Load SoundFonts into immutable bundles off-thread, swap only at a block
  boundary, and reclaim retired bundles off the audio thread
- [x] Remove callback-side logging/critical-section paths and add allocation
  instrumentation plus a source audit rejecting lock, debug-output, and UI calls
- [ ] Exercise live WASAPI device-buffer sizes from 16 through 8192 frames; the
  current sweep covers timing conversion, not device initialization/rendering
- [ ] Add reset and SoundFont-swap stress under ASan or an equivalent Windows
  memory-safety configuration

### Priority-Aware Event Ingress

- [x] Replace the single-producer ingress path with preallocated,
  sequence-numbered MPSC lanes
- [x] Partition 393,216 slots into 131,072 state, 131,072 loud, 65,536
  upper-medium, 32,768 medium, and 32,768 quiet entries
- [x] Timestamp and globally sequence events before admission/backpressure
- [x] Give state events and velocity 96-127 note-ons cancellable lossless
  backpressure
- [x] Use `WaitOnAddress`/wake epochs instead of indefinite producer spinning
- [x] Shed only note-ons in droppable velocity lanes; never intentionally shed
  note-offs, controller/state events, reset, or panic
- [x] Begin shedding at configurable pressure and continuously raise the
  admitted-velocity cutoff toward 95
- [x] Track intentional shedding globally and per velocity, separately from
  lateness and other telemetry
- [x] Fence queued note-ons at reset, CC120, and CC123 so priority-lane ordering
  cannot resurrect terminated sound
- [x] Verify four-producer MPSC integrity without duplicates, torn payloads, or
  lost events
- [x] Move raw MIDI decoding and remainder-preserving QPC-to-frame compilation
  to a persistent preallocated worker on modern multi-core systems; retain the
  synchronous callback path on single-core and XP targets
- [x] Feed compiled commands through a cancellable SPSC handoff and include
  both raw and compiled backlog in admission pressure
- [ ] Saturate every priority lane and test monotonic shedding, lossless-lane
  wakeup/backpressure, and shutdown cancellation
- [ ] Add bulk-packed MIDI ingestion and safe redundant-controller coalescing
- [ ] Measure and optimize the tens-of-millions-events-per-second path

### Sample-Accurate Scheduling

- [x] Convert QPC timestamps to integer output frames from a fixed epoch
- [x] Store future events in a bounded audio-thread-owned min-heap ordered by
  absolute frame and global ingress sequence
- [x] Append each callback's newly compiled commands as one batch and rebuild
  the scheduler heap once instead of performing one logarithmic insertion per
  event
- [x] Preserve deterministic equal-frame event ordering
- [x] Dispatch each equal-frame run through one batch callback while preserving
  every event and its global ingress order
- [x] Dispatch MIDI state changes and notes at exact render-frame boundaries
- [x] Clamp late events to the next writable frame and record lateness
- [x] Fast-forward missed output time after callback overruns and discard only
  obsolete note-ons so overload cannot become a permanent post-pause backlog
- [x] Drain obsolete ingress independently of the per-block admission budget
  and recover the newest still-on note per channel/key, with newer note-offs
  and termination fences winning, so extreme backlog converges to audible
  current state instead of permanent zero-voice output
- [x] Bound callback dispatch with `max_events_per_block`; excess remains
  ordered in the scheduler
- [x] Remove the old fractional pending-event/stable-sort execution path
- [x] Verify six simulated hours of QPC/frame conversion within one frame for
  block sizes from 16 through 8192 frames
- [ ] Add explicit sequence-gap/corruption detection beyond queue integrity
  tests
- [x] Provide `svms_v3_render`, an offline scheduling mode that processes every
  channel event without shedding or callback budgeting, memory-maps the SMF,
  streams parsed events through a bounded 128 MiB ring, and writes WAV/RF64
  incrementally with voice/steal/render-speed/ETA telemetry
- [x] Validate offline format-1 merging, running status, tempo changes, exact
  frame conversion, ring wrap, x64/x86 builds, and the 13,477,488-event Krash
  corpus using the production scalar/SSE2/AVX2 render backend

### Scalar, MIDI, and Voice Correctness

- [x] Rebuild channel state immediately for mid-block controller events and
  refresh active voice gains before rendering that frame
- [x] Include CC11 expression in active voice gain
- [x] Make CC120 silence only its channel immediately
- [x] Make CC123 release only its channel, including sustain-held voices
- [x] Make CC121 reset sustain and release notes that it held
- [x] Track overlapping same-key generations so note-off releases the oldest
  still-active generation without killing a later retrigger
- [x] Derive voice age from an absolute birth frame
- [x] Retire voices in O(1) with inverse active positions and append allocation
- [x] Preserve phase overshoot when one increment crosses multiple loop lengths
- [x] Correct SF2 loop-until-release behavior
- [x] Parse and additively merge SF2 `releaseVolEnv` across zone levels
- [x] Convert SF2 release timecents to an exact release-frame countdown that is
  continuous across callback boundaries and independent of starting gain
- [x] Keep the TSF-compatible 10 ms fallback/floor only for zero or
  near-instantaneous SF2 releases
- [x] Keep scalar correctness mode enabled by default
- [x] Implement SF2 per-voice pan in the live mixer
- [ ] Implement sostenuto, default modulators, filters, LFOs, chorus, and reverb

### 4096-Voice Full-Quality Scalar Optimization

- [x] Split sparse-event blocks into exact event-free frame spans
- [x] Keep persistent O(1) render-class lists for sustained loop/one-shot,
  looping transient/release, one-shot release/envelope, generic states, and
  independent steal tails; keep `activeList` for lifecycle/stealing only
- [x] Defer retirements and render-class transitions until a span completes so
  swap-removal cannot skip or duplicate a voice
- [x] Keep phase, gain, envelope, loop, fade, and release state in registers
  across each span and commit it once
- [x] Add aligned `RenderKernelSet`, `RenderSpanContext`, and
  `VoiceRenderClass` interfaces and move the sustained hot kernels into the
  dedicated `SVMSRenderKernels.cpp` translation unit
- [x] Add fixed 1/2/3/4-frame dense-event kernels plus specialized
  sustained-loop, one-shot, attack/decay, release, generic, and steal-tail
  paths without decimation, intrinsics, or relaxed floating-point mode
- [x] Remove production active-list copying and the frame-major dense fallback;
  retain the old frame-major renderer only as the differential test oracle
- [x] Maintain per-channel active indices for channel-local controller,
  sustain, termination, note-generation, and pitch-bend updates
- [x] Cache unbent phase increments and steady-state output gains; refresh only
  the affected channel on CC7/CC10/CC11/CC121
- [x] Preserve exact BASS-like steal scores/ties with a persistent sustained
  max heap plus an exact per-frame volatile heap for changing envelopes;
  validate every selected victim against an exhaustive oracle
- [x] Remove the shared current-frame age term from persistent heap keys, batch
  deferred voice setup into one candidate update, and replace same-frame heap
  roots in place without changing victim selection
- [x] Apply each prepared SF2 layer through one transactional voice setup and
  expose attack-frame control in the dense note-burst benchmark
- [x] Cache immutable preset/note/velocity region matches in an allocation-free
  direct-mapped table, cache committed channel presets, precompute the complete
  configured velocity-gain table, and cache channel pitch-bend ratios
- [x] Rebuild only the affected channel's derived controller state at an exact
  event boundary instead of recomputing all 16 channels per controller event
- [x] Make channel/key unlink O(1) with intrusive previous/next positions so
  dense same-key replacement never walks an entire retrigger generation
- [x] Track the oldest outstanding same-key generation directly, release only
  its adjacent SF2 layers on note-off, and avoid redundant default voice-state
  stores when prepared region setup immediately overwrites them
- [x] Precompute immutable SF2 region peaks, validation, per-key base pitch,
  envelope coefficients, attenuation, sustain, and pan while loading so the
  audio callback performs no diagnostic sample scans or transcendental region
  setup for ordinary note-ons
- [x] Add allocation-free rolling callback p95/p99/p99.9 and over-budget
  diagnostics without changing `DriverDebugInfoV1`
- [x] Extend `svms_v3_bench` with event stride, real mixed MIDI traffic,
  cycles/voice-sample, events/s, steals/s, render-class counts, consecutive
  deadline misses, MMCSS/FTZ/DAZ parity, and optional core affinity
- [x] Add full-velocity note-burst rates/key spreads and optional real-SF2
  layered-region matching to `svms_v3_bench`; characterize the supplied Krash
  corpus at 94k average and 792k peak note-ons/s
- [x] Verify 4096 voices for 60 seconds at 44.1 kHz/2048 frames on the
  i5-13600KF: sustained p99 36.72%, envelope p99 50.82%, release p99 37.42%
- [x] Differentially validate exact event order, active identities, steal
  victims/tails, phase/envelope/release state, and tolerant audio at buffers
  from 16 through 8192 frames
- [x] Reach modern-CPU 4096-voice event-stride-2 targets with no deadline
  misses: latest warmed AVX2 dense p99 16.67% and mixed-event p99 29.78%
- [x] Reduce the supplied 1000-voice Krash peak-rate synthetic profile from
  roughly 326% to 75.6% p99 without consecutive callback misses
- [ ] Run and pass the three Celeron 420 acceptance profiles on target hardware
- [ ] Complete live high-Hz listening validation for pitch, natural tails,
  CC120/123 termination, and callback-grid artifacts

## Reference and Regression Testing

- [x] Add deterministic scalar block-render tests
- [x] Differentially validate span and frame-major renderers with randomized
  events and buffer sizes from 16 through 8192 frames
- [x] Add tests for bank/program selection, compiled zones, layered regions,
  region validation, and the shipped `gm.sf2`
- [x] Add tests for pitch, overlapping retriggers, sustain, channel
  termination, exact release duration, loop wrapping, CC11, voice identity,
  BASS-like loudness/age stealing, velocity independence, and bounded fade tails
- [x] Add bounded scheduler ordering, queue wrap/capacity, four-producer MPSC,
  configuration lifecycle, and six-hour timing tests
- [x] Verify batch scheduler rebuilds and exact-frame batch dispatch preserve
  frame/sequence ordering
- [x] Build a streaming offline MIDI-to-WAV renderer with bounded parser/audio
  queues, progress telemetry, cancellation, and a scan-only analysis mode
- [ ] Render identical streams through V3 and BASSMIDI using the same SF2,
  sample rate, bend range, and disabled effects
- [ ] Measure the reference requirements: timing within one frame, exact event
  ordering, pitch within one cent, envelope checkpoints within 1 dB, waveform
  correlation, and RMS
- [x] Add reusable dense-note/event flood generators and analyze a local Black
  MIDI corpus without committing its large MIDI/SF2 assets
- [x] Characterize the Paprika 2/3 stress corpus without committing it: PNC2
  peaks at 554,240 note-ons/s and PNC3 at 942,960 note-ons/s; expose peak
  channel-event, note-on, and same-frame density in scan-only output
- [x] Recover BASSMIDI's runtime-unpacked voice-stealing paths: ordinary pool
  exhaustion ranks an envelope/control-derived effective level with an age
  bias, CPU overload batch-prunes by effective level alone, and audible stolen
  voices use a conditional reserve fade tail
- [ ] Establish scalar baselines and practical voice limits on a Celeron 420
- [ ] Establish modern-CPU event-flood and eventual 500K-voice baselines
- [ ] Add cycle/cache-miss profiling; callback duration, queue pressure,
  shedding, lateness, stealing, and retirement telemetry already exists

## Scalar Density and 500K Storage

- [ ] Raise the pool beyond the current 4096 fixed-array limit
- [ ] Implement segmented/paged SoA storage with bounded memory locality
- [ ] Separate hot audible state from cold metadata and release state
- [ ] Replace full active-list sorting with bucketed velocity/energy classes or
  hierarchical active tiles
- [ ] Add multi-rate advancement and compact scheduling for quiet/dormant tails
- [ ] Add audibility/energy estimates for mixing selection
- [ ] Add memory-budget configuration and predictable failure behavior
- [ ] Demonstrate sustained 500K logical voices in the stress harness
- [ ] Validate graceful overload degradation rather than stalls

## Parallel and SIMD Acceleration

- [x] Add an SSE2 backend boundary with scalar-equivalence coverage and retain
  the faster scalar short-span kernel where SSE2 lacks gather support
- [x] Add the AVX2 dense-span renderer with CPUID/OSXSAVE/XCR0 runtime
  detection, isolated compilation, and scalar/SSE2 fallback
- [ ] Per-worker mix buffers and tile-based render orchestration
- [ ] Optional worker-thread backend for modern multicore CPUs
- [ ] Prefetch strategies for decimated voices
- [ ] Ensure every accelerated path retains scalar fallback coverage

## Formats, Tools, and Productization

- [ ] SFZ and DLS support
- [ ] Shared-memory telemetry and performance graphs
- [ ] Configurator, SoundFont browser, and profile import/export
- [ ] Record-to-WAV support
- [ ] WASAPI exclusive and ASIO backends

## Non-Negotiable Constraints

- The scalar path remains functional and profiled on legacy hardware.
- The 500K target is evaluated on modern multicore hardware, not the Celeron
  420 compatibility tier.
- The audio callback does not allocate, take general-purpose locks, display UI,
  or emit debug output.
- Voice overload and event overload have explicit bounded behavior.
- Optimizations must not reintroduce same-key voice recycling.
- Timing remains sample-accurate within the configured overload policy.
