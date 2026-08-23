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
- [x] Full-quality rendering throughout the 4096-voice validated baseline;
  selectable capacity now extends to 524,288 voices
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
- [x] Select one to 64 total voice-render threads through
  `synth.render_threads`/`SVMS_RENDER_THREADS`; zero requests a conservative
  automatic count and one preserves the original single-thread path
- [x] Select a named WASAPI render endpoint through `audio.device` or
  `SVMS_AUDIO_DEVICE`; a missing configured endpoint fails safely instead of
  falling back to an unintended default output
- [x] Write `audio.device: "default"` on first creation and resolve that
  sentinel through the current Windows default render endpoint, while retaining
  compatibility with older empty-device configurations
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
- [x] Add a native settings query/merge-patch API that validates recognized
  values, round-trips unknown fields, uses the configuration mutex, and commits
  through atomic replacement
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
- [x] Size all five sequence-numbered MPSC lanes at runtime from
  `events.ring_capacity`, retaining the 1/3 state, 1/3 loud, 1/6
  upper-medium, 1/12 medium, and remaining quiet proportions
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
- [x] Compile commands into immutable 8,192-event pages, stably ordered by
  absolute frame and ingress sequence, and publish/recycle page descriptors
  through preallocated lock-free SPSC index queues
- [x] Preserve global sequence order while draining the five FIFO lanes with a
  persistent lane-head merge; include raw, paged, and scheduled backlog in
  admission pressure
- [x] Rotate fairly across priority lanes during compilation so saturated
  state traffic cannot indefinitely starve full-velocity note-ons
- [x] Suppress per-event compiler wakeups and amortize producer pressure scans
  without weakening cancellable lossless backpressure
- [ ] Saturate every priority lane and test monotonic shedding, lossless-lane
  wakeup/backpressure, and shutdown cancellation
- [ ] Add bulk-packed MIDI ingestion and safe redundant-controller coalescing
- [x] Measure and optimize the tens-of-millions-events-per-second path: the
  accepted 12M-event/s fixture compiles around 126M events/s and performs
  callback ordering around 622-689M events/s (roughly 0.4-0.5% of a
  44.1-kHz/2,048-frame callback)

### Sample-Accurate Scheduling

- [x] Convert QPC timestamps to integer output frames from a fixed epoch
- [x] Store future events in immutable sorted pages and select the next exact
  `(targetFrame, ingressSequence)` through a fixed-capacity page-head winner
  tree; importing/advancing work is bounded by page count and payloads are not
  recopied or rescanned
- [x] Extract only pages' events due before the callback end into the existing
  preallocated render-event buffer, returning exhausted pages through the
  lock-free recycle queue
- [x] Compact scheduled commands to 16 bytes and size the scheduler/event
  working sets from the configured ring and per-block capacities
- [x] Remove the former 393,216-event validation/allocation ceiling: the
  compiler handoff, scheduler, and callback buffer now use the requested
  runtime capacity up to the process address-space/allocation limit, with a
  safe low-memory fallback when the request cannot be allocated
- [x] Add a production-path event-pipeline benchmark covering MPSC admission,
  frame compilation, chunk ordering, SPSC handoff, callback merge, and extract
- [x] Preserve deterministic equal-frame event ordering
- [x] Dispatch each equal-frame run through one batch callback while preserving
  every event and its global ingress order
- [x] Batch consecutive equal-frame note-offs for one channel/key through the
  exact oldest-generation operation without changing release multiplicity
- [x] Dispatch MIDI state changes and notes at exact render-frame boundaries
- [x] Clamp late events to the next writable frame and record lateness
- [x] Fast-forward missed output time after callback overruns and discard only
  obsolete note-ons so overload cannot become a permanent post-pause backlog
- [x] Drain obsolete ingress independently of the per-block admission budget
  and recover the newest still-on note per channel/key, with newer note-offs
  and termination fences winning, so extreme backlog converges to audible
  current state instead of permanent zero-voice output
- [x] Compact only already-late note-off history into counted per-key batches,
  preserving stereo/layer play groups and the current timeline while preventing
  dead history from filling an entire callback and starving fresh note-ons
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
- [x] Treat every physical SF2 region sharing one `playIndex` as an atomic
  steal group, so stereo and layered notes cannot lose only one channel while
  mono notes continue to consume and retire one physical voice
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
- [x] Remove DC and subsonic output with a stateful 3 Hz post-limiter
  high-pass, fused into the limiter loop and shared by live/offline output
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
- [x] Transfer saturated stable-mono replacements directly between channel-
  index pages, avoiding an unindexed intermediate state and redundant unlink/
  relink bookkeeping. Corrected 5.5M-NPS cost falls 1.8% at 300 voices and is
  slightly lower at 1,000 voices, with byte-identical PNC3 output
- [x] Reuse the saturated fast path's known sustained render class while
  applying voice fields, skipping duplicate loop and envelope classification.
  Corrected 5.5M-NPS cost falls another 0.8% at 300 voices and 1.2% at 1,000
  voices, with byte-identical PNC3 output
- [x] Cache unbent phase increments and steady-state output gains; refresh only
  the affected channel on CC7/CC10/CC11/CC121
- [x] Preserve exact BASS-like steal scores/ties with a fixed-leaf sustained
  winner tree plus an exact per-frame volatile heap for changing envelopes;
  validate every selected victim against an exhaustive oracle
- [x] Encode each stable candidate's float score and active-position tie into
  one exact monotonic 64-bit winner key, reducing hot tournament comparisons
  to a single integer compare; the 2,000-voice Morphine 943K-note profile
  improves another roughly 2.5% with byte-identical full PNC3 output
- [x] Remove the shared current-frame age term from persistent heap keys, batch
  deferred voice setup into one candidate update, and replace same-frame heap
  roots in place without changing victim selection
- [x] Replace saturated matching stereo/layer groups in their existing winner-
  tree leaves and refresh the union of their paths once; preserve exact group
  victims, both 64-sample tails, and future victim ordering
- [x] Keep delay/hold/attack voices in the persistent exact steal tree because
  their protected target-gain score is time-invariant; reserve per-frame
  volatile rebuilding for decay and release only
- [x] Batch 1-4-frame looping attack/decay spans through unrolled scalar
  render-class kernels shared by every backend; on the 2,000-voice Morphine
  943K-note profile this cuts transient synthesis cycles by roughly 40% while
  preserving scalar arithmetic order and exact envelope transitions
- [x] Bypass the mutable per-sample envelope state machine when an attack or
  decay counter cannot expire inside its 1-4-frame span; the Morphine 943K-note
  profile drops another 14-15% of synthesis cycles and about 7.5% total work,
  with byte-identical PNC3 output on scalar-compatible arithmetic
- [x] Treat audio-thread-owned render-class membership as the kernel contract
  and remove the redundant full transient-class validation sweep from every
  1-4-frame span; the same Morphine profile drops another roughly 11% of
  synthesis cycles with byte-identical PNC3 output
- [x] Have transient kernels report only voices that actually cross an
  envelope-stage boundary, eliminating copy/reclassification passes over every
  unchanged attack/decay voice; the 2,000-voice Morphine profile drops another
  roughly 29% of synthesis cycles and returns below its real-time deadline
- [x] Drain priority ingress as bounded FIFO lane runs before exact scheduling,
  removing artificial event-by-event lane inversions while preserving final
  absolute-frame/ingress-sequence order; the 262,144-event dense benchmark
  improves from 28.86M to 121.01M compiled events/s (4.19x), with byte-identical
  full PNC3 + Morphine Piano output
- [x] Apply each prepared SF2 layer through one transactional voice setup and
  expose attack-frame control in the dense note-burst benchmark
- [x] Prepare complete layered note launches before pool mutation, cache the
  common eight-layer launch plans by SoundFont/preset/pitch/note/velocity, and
  retain an explicit legacy-versus-transactional benchmark control
- [x] Launch directly from immutable cached SF2 plans and pass the per-note
  play generation separately, removing full-plan copies, repeated region
  checks, and scratch patching. The modeled 5.5M-NPS one-layer path drops
  total work 8-9%; the 943K-NPS two-layer Morphine path drops 5.2%
  with identical victims, state, and full PNC3 output
- [x] Reserve distinct exact victims for every layer in a saturated fallback
  transaction; this prevents a deferred winner-tree leaf from being selected
  twice, preserves stereo play groups, and enables the existing in-place group
  replacement path. The 2,000-voice Morphine 943K-note profile improves from
  97.5% to 73.7% p99, with all 2,000 physical voices correctly grouped
- [x] Apply the exact in-place saturated replacement transaction to mono
  SoundFonts as well as layered instruments. At 5.5M one-layer note-ons/s and
  512 frames, 300-voice total cycles fall about 20% and clean-run p99 falls
  from 121.8% to 100.6%, with an exact 512-victim oracle and byte-identical
  full PNC3 output
- [x] Specialize stable mono in-place launches as one lifecycle/configuration
  transaction, bypassing the generic eight-layer reservation and commit loops.
  Under concurrent game load, 5.5M-NPS median callback use falls about 5% at
  300 voices and 6% at 1,000 voices with exact predecessor audio
- [x] Specialize the exact newborn stable-key calculation and force the compact
  key encoder, binary winner path, and mono replacement transaction inline on
  MSVC and MinGW. Fixed-core 6M-note/s medians improve about 2%, while the
  sampled winner/key segment falls roughly 8-15% with identical oracle victims
- [x] Initialize only the observable lifecycle fields during saturated stable-
  mono replacement, removing placeholder voice state and redundant index work.
  On an otherwise idle development machine, corrected 5.5M-NPS cost falls
  2.7% at 300 voices and 2.5% at 1,000 voices, with byte-identical PNC3 output
- [x] Size the logical exact-stealing tournament to the next power of two
  above configured polyphony while retaining fixed preallocated storage. A
  300-voice pool now updates 9 levels instead of 12, reducing steal/index
  cycles 5.8% and total 5.5M-NPS work 2.9%, with identical victims and audio
- [x] Make exact tournament selection branchless and send single-voice updates
  directly down one winner path. At 5.5M one-layer note-ons/s this reduces
  steal/index work about 10-11% and total callback work about 5-6% at both 300
  and 1,000 voices, with byte-identical full PNC3 output
- [x] Store complete ordered 64-bit winner keys in the tournament nodes, so
  each level is one integer maximum with no indirect leaf-key loads. Corrected
  5.5M-NPS cost falls another 6.3% at 300 voices and 7.1% at 1,000 voices;
  the root still selects the exact active-position tie winner and PNC3 is
  byte-identical
- [x] Decode the exact stable score and active position directly from the
  tournament root, removing the redundant stable-candidate array. This trims
  VoiceManager by 48 KiB and reduces corrected 5.5M-NPS cost another 2% at
  both 300 and 1,000 voices with byte-identical PNC3 output
- [x] Remove the defensive full volatile-list scan after consulting the exact
  current-frame volatile heap. Its membership is maintained one-to-one by
  every link, unlink, and reserved-root transaction. On the 1,000-voice 6M
  chopped-note corpus, realized MIDI-event throughput rises from 3.57M/s to
  5.18M/s (45%), callback work falls about 31%, and sampled victim selection
  falls from roughly 865 to 198 cycles without changing the exhaustive victim
  oracle
- [x] Pack volatile-heap priority and active-position ties into the same exact
  monotonic 64-bit key used by the stable tournament, with handles kept in a
  separate compact array. A release-heavy equal-frame oracle covers repeated
  heap removals and repairs. On the 1,000-voice 6M chopped-note corpus,
  realized throughput rises from about 5.10M/s to 5.86-5.90M/s and cost falls
  from 184.98 to 160.23-160.56 cycles per voice-sample without changing victims
- [x] Bypass the stable mono replacement probe when the packed volatile root
  is already the exact winner. Release-heavy launches no longer select and
  reserve the same volatile victim twice; the 6M chopped-note corpus reaches
  5.94-6.00M events/s at 157.22-158.93 cycles per voice-sample with unchanged
  exhaustive-oracle victims
- [x] Extend the saturated mono in-place transaction to exact volatile-heap
  victims. It retains the physical slot while preserving the selected victim,
  64-frame tail, lifecycle indices, and replacement tree leaf. Sampled launch
  cost falls from about 1,146 to 921 cycles, while uninstrumented throughput
  reaches 6.16-6.25M events/s at 151.00-152.92 cycles per voice-sample
- [x] Keep exact stable/volatile winner selection entirely in packed-key form,
  removing float decode and generic candidate reconstruction from every steal.
  The same corpus reaches 6.24-6.28M events/s at 150.45-151.40 cycles per
  voice-sample, another small 0.5-1% gain
- [x] Replace per-steal scans of the 50 outgoing fade tails with an exact
  once-per-frame minimum heap and cached tail levels
- [x] Pack each outgoing-tail heap level and list-position tie into one exact
  64-bit key. This cuts tail admission bookkeeping about 25% and improves the
  full 5.5M-note/s launch workload about 2.6-2.7% without changing PNC3 audio
- [x] Resolve the packed root's tail slot only after its level passes the
  strict-louder admission test; the common full-reserve rejection path gains
  another roughly 0.7-1.0% at 5.5M note-ons/s with identical output
- [x] Size outgoing tail SoA, lifecycle lists, and renderer scratch to the
  fixed 50-tail reserve instead of maximum voice capacity; this removes about
  271 KiB at 4,096 voices and roughly 33.5 MB from a future 500K layout while
  improving the 2,000-voice Morphine 943K-note p99 from about 71-72% to a
  clean-run 66-68%, with byte-identical full PNC3 output
- [x] Replace the 16 full-capacity channel voice arrays with an allocation-free
  pool of 64-handle blocks while preserving exact append/swap-remove traversal;
  this removes 236,096 bytes at 4,096 voices and about 28 MiB from an equivalent
  500K layout, with unchanged sustained performance and byte-identical PNC3
  output
- [x] Store all seven render-class memberships in a shared allocation-free pool
  of 1,024-handle kernel tiles instead of seven full-capacity arrays; exact
  render ordering crosses tile boundaries, 4,096-voice speed is unchanged, and
  an equivalent 500K layout avoids about 10.5 MiB
- [x] Allocate all primary `VoiceSoA` fields in one 64-byte-aligned block sized
  to configured polyphony. The default 1,000-voice manager shrinks from
  1,144,512 to 607,712 bytes (46.9%), 4,096-voice rendering is unchanged, and
  full PNC3 + Morphine Piano output remains byte-identical
- [x] Size renderer class-transition and deferred-retirement scratch to
  configured polyphony during initialization. The default allocation shrinks
  from 65,856 to 16,320 bytes (75.2%) without callback allocation or an audio
  change
- [x] Cache immutable preset/note/velocity region matches in an allocation-free
  direct-mapped table, cache committed channel presets, precompute the complete
  configured velocity-gain table, and cache channel pitch-bend ratios
- [x] Rebuild only the affected channel's derived controller state at an exact
  event boundary instead of recomputing all 16 channels per controller event
- [x] Cache each channel's pan/volume/expression mix scales during that rebuild,
  reducing note-launch gain setup from three multiplies to one per side; the
  1,000-voice 5.5M-note/s path gains about 1.3% with byte-identical PNC3 audio
- [x] Batch channel-wide steal-key refreshes into one contiguous winner-tree
  rebuild so dense CC7/CC10/CC11 traffic does not thrash random tree paths
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
- [x] Sample dense-event cycle breakdowns instead of reading the timestamp
  counter around every event; this removes the profiler's former 2x callback
  distortion while retaining scaled region, launch, dispatch, and steal costs
- [x] Add benchmark-only sampled decomposition of saturated mono replacement
  into victim lookup, tail capture, lifecycle indices, configuration stores,
  and winner-tree commit, with no instrumentation in the production DLL
- [x] Add an explicit chopped-note corpus plus opt-in logical/physical launch
  churn classification. At 1,000 voices and 6M requested note-ons/s with
  five-frame notes, all 6,130,080 measured victims were older releasing voices
  and none were born on the same output frame, rejecting shadow launch as the
  next useful optimization; exact volatile selection is the measured target
- [x] Make the dense note benchmark reuse complete direct-mapped immutable
  launch plans like the live driver, rather than zeroing a 512-pointer array,
  copying cached regions, and rebuilding synthetic setup on every note. Its
  corrected 5.5M-NPS median is about 50% at 300 voices and 57-60% at 1,000
- [x] Capture the diagnostic window's detailed last-SF2-voice probe once per
  callback instead of once per successful note-on. Exact lifetime counters and
  public voice/steal/render telemetry remain per event, while a 5.5M-NPS stream
  avoids roughly 110 million redundant detail stores per second
- [x] Fuse diagnostic peak collection into the mandatory stereo interleave
  pass, and omit peak arithmetic entirely when diagnostics are disabled; this
  removes one complete left/right mix-buffer read pass from every callback
- [x] Add full-velocity note-burst rates/key spreads and optional real-SF2
  layered-region matching to `svms_v3_bench`; characterize the supplied Krash
  corpus at 94k average and 792k peak note-ons/s
- [x] Make the benchmark's transactional mode exercise complete two-to-eight-
  layer launches instead of silently falling back to legacy per-layer steals
- [x] Verify 4096 voices for 60 seconds at 44.1 kHz/2048 frames on the
  i5-13600KF: sustained p99 36.72%, envelope p99 50.82%, release p99 37.42%
- [x] Differentially validate exact event order, active identities, steal
  victims/tails, phase/envelope/release state, and tolerant audio at buffers
  from 16 through 8192 frames
- [x] Reach modern-CPU 4096-voice event-stride-2 targets with no deadline
  misses: latest warmed AVX2 dense p99 16.67% and mixed-event p99 29.78%
- [x] Reduce the supplied 1000-voice Krash peak-rate synthetic profile from
  roughly 326% to 75.6% p99 without consecutive callback misses
- [x] Pass the warmed 60-second 2000-voice PNC peak profile at 943k note-ons/s:
  AVX2 p99 34.73%, p99.9 40.65%, and no consecutive deadline misses; the
  500k-note profile reaches 26.55% p99. The non-gating 5.5M-note stretch case
  drops from roughly 206% to 105.31% p99
- [x] Validate the paged scheduler live in Ziggy with Project CF-162 at 1,000
  voices: PNC2 and Krash do not pin the callback batch or collapse to zero
  voices, and PNC3 holds scheduler cost below 1% through its 943K-NPS peak
- [ ] Eliminate the remaining PNC3 peak streak: the current live run reaches
  p99.9 107% with a maximum four consecutive deadline misses while event
  dispatch/launch—not scheduling—is saturated
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
  BASS-like loudness/age stealing, velocity independence, atomic stereo/mono
  steal groups, and bounded fade tails
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

- [x] Raise the selectable pool beyond 4096 with capacity-sized voice and
  lifecycle indices (524,288 logical ceiling; practical limits remain to be
  characterized)
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
- [x] Add persistent per-worker mix buffers and deterministic 256-handle tile
  orchestration for sustained-loop spans, with no callback allocation and a
  fixed-order audio-thread reduction
- [x] Add an optional one-to-64-thread voice renderer for modern multicore
  CPUs; keep exact-frame MIDI dispatch/lifecycle ownership on the audio thread
  and bypass workers for tiny spans where synchronization would cost more
- [x] Replace round-robin span ownership with dynamically claimed logical
  256-handle tiles, generation-counter wakes on modern Windows, fixed tile
  reduction order, and the XP event-wait fallback
- [x] Add the exact 128-frame dense planner with double-buffered mutation
  plans: chunk N synthesis overlaps chunk N+1 event/allocation planning while
  every event retains its original frame and ingress order. At 2,000 voices
  and 943K note-ons/s, warmed AVX2 p99 is 15.29% with four threads and 16.51%
  with eight, versus 32.97% serial; x64/x86/XP suites pass
- [x] Add opt-in benchmark coverage telemetry for dense-plan rejection,
  execution fallback, exact span-length buckets, sparse voice-samples, and
  worker rejection reasons. An 8,192-voice/512-boundary fixture proves the
  current gap: all 22 callbacks fail only the event-density gate and all
  369,098,752 sustained voice-samples remain serial in four-frame spans
- [x] Replace the dense planner's record-count gate with measured rejected
  synthesis work after applying the existing span-worker gates. A 4M
  voice-sample threshold preserves the faster sparse path at 2,000 voices,
  while 8,192 voices across 512 boundaries improve from 35.96% to 28.98% p99
- [x] Bound dense mutation storage by the union of event-affected channel
  populations instead of `distinct frames * max voices`, retaining full-pool
  bounds for resets and note launches. The 8,192-voice/683-boundary fixture no
  longer rejects or falls back and improves from 48.48% to 33.09% p99
- [ ] Prefetch strategies for decimated voices
- [ ] Ensure every accelerated path retains scalar fallback coverage

## Formats, Tools, and Productization

- [ ] SFZ and DLS support
- [x] Preserve the 512-byte RuntimeLink V2 ABI while exposing scheduler load,
  event-dispatch load, raw ingress, compiled-page pressure, and scheduled
  backlog through its five reserved words
- [x] Display paged-pipeline pressure and separate queue-capacity versus
  per-callback event-budget controls in the configurator
- [ ] Profile import/export
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
