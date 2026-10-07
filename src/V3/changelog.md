# SVMS V3 — running changelog

Low-key running log, updated with every landed change. The root
`CHANGELOG.md` carries the polished per-version entries I publish when I cut
a release; this file is where changes land first, raw, under "Unreleased",
until I move them into a release section.

Format: newest first, one bullet per landed change, matching the commit's
`type(v3): one-liner` style plus a line of context where it helps.

## Unreleased

- 2026-10-07 fix(v3): build the canonical engine as C++17. Span/Path shims in
  Engine/CanonicalCompat.h replace std::span and std::filesystem (GCC 7 has only
  the Filesystem TS); atomic wait falls back to a mutex + condition variable.
- 2026-10-07 ci(v3): save the Linux ccache even when the job fails; retry apt
  actions/cache only saves on success, so a red Linux job never warmed its
  cache. Restore and save are separate steps now (save runs always), the
  container makes its root-owned cache readable from an exit trap, and apt
  retries downloads (the last run died on an archive.ubuntu.com timeout).
- 2026-10-07 fix(v3): restore the Linux build (POSIX worker pool, GCC 7 filesystem)
  The POSIX worker pool had drifted from the Windows one: old indexed-job
  signature, no bus planes, kernel refusals silently dropped, and span jobs
  writing retirements into one shared array from every thread. It now mirrors
  the Windows job semantics line for line (futex sync kept). SVMSSoundFont.cpp
  falls back to the Filesystem TS on GCC 7 (Ubuntu 18.04 has no <filesystem>).
- 2026-10-07 fix(v3): make per-voice phase rotation exactly analytic
  Two things kept rotation from being a pure phase shift. The allpass
  "quadrature splitter" (still used by Random) was never in quadrature: its
  branches were 7-170 degrees apart, so each voice got an angle-dependent
  filter. And the Hilbert companion was built per whole sample slice, so over
  a loop it stepped at every wrap (a click at the loop rate) and the voice's
  level depended on the angle. The new SVMSHilbertPair.h builds the loop as a
  periodic transform, crossfades attack/tail into it and guards one-shots;
  modes 1/2/4 always use the exact pair (Random = per-voice sweep rate and
  direction), and the splitter is gone. Correctness suite green again
  (was red since ce65158); new loop tests fail on the old builder.
- 2026-10-07 feat(v3): canonical engine phase 2 (WIP) + launch-hot VoiceRow
  Unfinished work pushed as-is. src/V3/Engine/ holds the canonical engine,
  wired only into svms_v3_render behind a legacy|canonical selector (see
  CANONICAL_ENGINE_MIGRATION.md). Also in here: the V3 voice pool packs its
  launch bookkeeping fields into one 64-byte VoiceRow per voice; ghost copies
  copy the row. Correctness suite unchanged (only the known Hilbert failure);
  not benchmarked yet.
- 2026-10-07 ci(v3): warm compiler caches for the Windows and Linux builds
  Windows CI runs build_v3.bat under sccache (GitHub Actions cache backend);
  CMake sees the launcher and switches to /Z7 + no driver PCH so every object
  is cacheable. The Linux (Ubuntu 18.04 container) job uses ccache with its
  directory saved by actions/cache. The first run fills the caches.
- 2026-10-07 build(v3): precompile SVMSDriverInternal.h for the driver sources
  The 10 driver translation units each re-parsed the big engine headers
  (~1.2 s apiece). A PCH (SVMS_DRIVER_PCH, driver files only; kernels/ASIO/etc.
  skip it) brings a header touch to 5.7 s (was 10.5) and a single-file edit
  to ~2 s. Under a compiler-cache launcher the PCH defaults off and /Zi becomes
  /Z7, so sccache can cache everything in CI.
- 2026-10-07 build(v3): compile the engine once for winmm.dll and SVMSAPI.dll
  Both DLLs used to compile every engine source separately. They now link one
  `svms_engine` OBJECT library; only SVMSFrontWinMM.cpp is still built per DLL,
  because svmsapi compiles it with _WINMM_. Export and import tables of both
  DLLs are identical. Touching SVMSDriverInternal.h rebuilds in 10.5 s (was 23 s).
- 2026-10-07 fix(v3): value-initialize SF2Data with () instead of {}
  `new SF2Data{}` aggregate-initializes 65536 SFSampleRegions that have member
  initializers, and cl.exe pays ~3 s plus a lot of memory for it in every TU
  that includes SVMSStandaloneSynth.h (it ran a full-parallel build out of
  RAM). `new SF2Data()` gives the same zeros-plus-defaults object.
- 2026-10-07 refactor(v3): split SVMSDriver.cpp by area
  The 10k-line driver is now SVMSDriverInternal.h (class Driver + shared
  helpers) plus SVMSDriver/RuntimeLink/SoundFont/Backend/Ingress/Render.cpp
  and the front ends SVMSFrontWinMM/Native/KDMAPI/Bass.cpp + SVMSDllMain.cpp.
  Code moved verbatim; the only edits are linkage (shared file-scope statics
  became inline, four native offline functions lost `static` because the BASS
  shim calls them). Export tables and test output are identical. A one-area
  edit rebuilds in ~3-4 s instead of ~11.5 s; touching the internal header
  costs ~23 s at -j 2. SVMSNativeOffline.h is included only by the native
  front end: its StandaloneSynth `new SF2Data{}` is expensive for cl.exe and
  ran a full-parallel build out of memory when every file included it.
- 2026-10-07 refactor(v3): split RenderEvent out of SVMSRenderScalar.h
  SVMSEventScheduler.h included the whole renderer (and with it the voice
  manager) just for `RenderEvent`. The event-pipeline headers now include the
  40-line SVMSRenderEvent.h instead. No code changes.
- 2026-10-07 refactor(v3): move the driver reverb into SVMSReverb.h
  First cut of the monolith split. `ReverbState` was 1,460 self-contained lines
  in the middle of SVMSDriver.cpp; it moved verbatim. The DLL export tables
  and test output are identical before and after.
- 2026-10-07 perf(v3): trim whole-voice plan loop (O(1) release-op drop, slim ghost copy, same-frame note batching)
  The serial plan is 93-100% of block time at multi-M NPS. I made the
  deferred-release op drop O(1) (per-handle index), copy only the fields
  ghosts actually read on each steal, and hand same-frame note runs to the
  batch dispatcher in one call so the driver's exact-frame batching engages
  (ordinal semantics unchanged). I also fixed a bench artifact
  (8 VoiceConfiguration ctors per note-on) and added the wv_cycles split.
  Whole-voice test output is byte-identical. p50 250v/15M 152 -> 118,
  8192v/5M 124 -> 112; most of that is the bench fix. The remaining wall is
  victim-row cache misses in LaunchVoiceGroup.

- 2026-09-16 fix(v3): honor SIMD worker fallback for phase-rotated voices
  The render worker pool now preserves the documented class-kernel contract:
  when AVX2/SSE2 refuses a complete job without mutation, the matching scalar
  class kernel consumes that same private worker slice immediately. This fixes
  rotation-enabled transient/release batches being silently dropped, which
  froze envelope/release progression and left voices stuck active. Random mode
  also keeps its intended jittered allpass+sweep path instead of collapsing
  into the same exact-Hilbert form as Sweep.


- 2026-09-16 fix(v3): make Analytic phase rotation use the real Hilbert pair
  I build the analytic companion with every realtime SoundFont bundle so live
  mode switches cannot silently fall back to the quadrature approximation,
  wire the same pair through standalone/offline rendering, and order filtered
  rotated voices as Hilbert rotation -> low-pass so the unfiltered companion
  is never combined with a differently filtered real component. Rotation still
  refuses whole-voice/dense paths where their state model cannot represent it;
  those paths fall back for correctness rather than approximating the DSP.

- 2026-09-15 feat(v3): add initial SFZ instrument support
  I compile external WAV-backed SFZ regions into the existing V3 sample and
  region pipeline, including inheritance, mapping, tuning, gain/pan, loops,
  amp envelopes, group/off_by, recursive configurator discovery, and tests.

- 2026-09-15 feat(v3): honor SF2 exclusive classes on note launch
  I track active exclusive-class voices by MIDI channel and choke the prior
  complete play group at the exact note-on frame without self-choking layers.

- 2026-09-15 perf(v3): batch matching whole-voice render segments
  I dispatch consecutive voices with the same exact start frame and render
  class through one class-kernel call when no release, row-op, or vibrato
  boundary splits them, preserving handle order and scalar mix order.

- 2026-09-14 feat(v3): play extended 256-key MIDI and add selectable 31EDO tuning
  I preserve full-byte note identities through playback and offline decoding,
  extend top SoundFont zones, and keep normal tuning unchanged. 31EDO is
  restart-applied (key 155 = middle C); percussion keeps its instrument keys.
- 2026-09-14 fix(v3): reject inaccessible SysEx buffers before parsing
  I copy caller buffers safely so Ziggy cannot crash the DLL by passing -1
  as a long-message data pointer; KDMAPI reports an invalid parameter.


- 2026-09-14 fix(v3): restore the channel activity bars after the label-width
  alignment fix pushed the right-column meters outside their table cells.
  Per-channel gain reduction now ramps down over 0.5 ms instead of jumping
  in one sample; the master limiter catches the brief attack overshoot, so
  threshold crossings stop clicking without adding lookahead latency.

- 2026-09-14 fix(v3): clear capacity-strided channel buses per plane
  instead of as a packed active-frame prefix. Variable-size audio callbacks
  could otherwise re-sum stale samples from earlier blocks, sounding like
  stuck notes and compounding distortion. The live threshold glide now
  advances once per frame and is shared consistently by all 16 channels;
  channel 9's configurator meter reserves the same two-digit label width as
  channels 10-16 so its bar aligns with the column.

- 2026-09-14 fix(v3): channel-limiter live updates never reached the engine
  when no grouped change was pending — FlushLiveChanges sent ApplyLiveConfig
  with an empty group mask (driver rejected "Invalid argument - empty group
  mask") and the dedicated SetChannelLimiter command only ran behind a
  success return. ApplyLiveConfig is now skipped entirely when the grouped
  mask is zero, so per-channel-limiter-only changes flush on their own
  command. Same screenshots: the fixed 24 dB GR scale pegged half the
  channel grid full — Black MIDI buses peak 20-40 dB above full scale, so
  the grid now steps a shared display max (24/48/96/192) off the worst
  channel with a scale caption, mirroring the limiter page.

- 2026-09-14 fix(v3): Per-Channel Limiter page cleanup from the owner's
  screenshots — the enable toggle rendered its raw "##cl_enabled_switch"
  ImGui id (ToggleSwitch draws its label; pass "ENABLED"), the channel
  activity grid went from a sparse 4x4 vertical-bar table to a compact
  2-column x 8-row layout with tight horizontal GR bars, and the topology
  description paragraph is gone (release knob gains a tooltip instead).

- 2026-09-13 feat(v3): per-MIDI-channel limiter (opt-in, default OFF) — 16
  stereo channel buses limited independently, then summed into the master
  chain. Purely POST: RenderBlock gains optional channelBusLeft/Right plane
  tables; when the feature is off nothing runs and output is bit-identical
  (boundary #2 honoured by construction). The limiter itself is classic
  zero-latency (SVMSChannelLimiter.h): per-channel peak envelope with
  instant attack + one-pole release toward the running peak (mirror of the
  master Classic detector), 4 dB soft knee, gain applied from the envelope
  of the same sample — above the knee a bus never exceeds the threshold
  exactly, inside the knee the worst case is threshold*knee. Deliberately
  NOT predictive: 16 lookahead lines would add latency to guard transients
  the master limiter already owns; this stage is about one runaway channel
  eating everyone else's headroom. Plumbing: RenderSpanContext grew
  channelBusLeft/Right (aggregate initialisers value-init them null, legacy
  sites untouched); class kernels select the voice's plane per voice (a
  voice's MIDI channel is fixed for life — one pointer select per span, no
  per-frame scatter); worker pool gained lazy per-job bus planes with a
  per-channel deterministic merge; dense tiles re-batch per job in bus mode
  (390 private 16-plane buffers for a 100k pool would be absurd); the AVX2
  voice-batched 1-7-frame short kernels refuse in bus mode (eight lanes =
  eight channels into one frame position — boundary #7 refuse, scalar
  per-voice fallback takes it); SSE2/scalar short batch write planes
  directly. Driver allocates the buses alongside the mix buffers (never in
  the callback), limits+sums after RenderBlock, before reverb/master
  limiter. Live via a dedicated SetChannelLimiter wire command
  ("enabled;threshold;releaseMs" in the command text area — the live-state
  V2 struct is ABI-pinned) with block-boundary adoption and threshold
  glide; telemetry grew prefix-compatibly 512 -> 704 bytes (per-channel GR
  + pre-limit peak, structSize-driven offsets handle both sizes — the ABI
  smoke test pins the append). Configurator: new Effects > Per-Channel
  Limiter page (toggle, threshold/release knobs, 4x4 channel GR grid),
  JSON section channel_limiter. Tests: TestPerChannelLimiterDifferential —
  limiter unit properties (ceiling, exact quiet passthrough, step response,
  release), engine-vs-oracle bus parity (scalar 3.7e-9; AVX2 worst 2.3e-3 =
  the documented steal-storm victim-flip class, budget 1e-2), both bus
  paths (whole-voice via even blocks, dense/sparse via AllSoundOff-refused
  odd blocks), direct-mix continuity after bus blocks. ctest 15/15, XP
  13/13. Bench (AVX2, 6 threads, 2048 frames): sustained @8192 0.493 ->
  0.519, note-burst @5000 0.568 -> 0.623, chopped-notes @5000 0.605 ->
  0.683 cycles/voice-sample with the feature ON; OFF matches baseline.

- 2026-09-13 perf(v3): CC1 vibrato joins the whole-voice fast path. The old
  "any channel modDepth > 0 -> refuse whole-voice + dense, cap sparse spans
  at 64 frames" gate cost 9.2x on modulated material (chopped-notes + CC1:
  5.25 vs 0.57 cycles/voice-sample) because ONE mod-wheel touch poisoned
  every subsequent block. Now the per-row AVX2 kernels rebuild the 64-frame
  LFO control window internally (scalar advance-then-use triangle +
  degree-5 exp2 Taylor instead of powf, windows never straddle a fast
  chunk), and CC1/CC121/channel-pressure report post-rebuild depth through
  a new vibrato row-op (kind 2) that the worker items track alongside bend
  ratio cursors. Whole-voice takes vibrato only on the AVX2 backend —
  scalar/SSE2 keep the legacy path bit-identical (boundary #2), and the
  sparse path keeps AdvanceVibratoSpan as the reference. Whole-voice-vs-
  sparse waveform parity carries the documented control-rate drift (window
  boundaries differ): TestWholeVoiceVibratoDifferential measures 1.7e-2
  saturating over 8 blocks, budget 3e-2. Bench: CC1 5.25 -> 0.97 (1.4x over
  baseline). Also: per-channel vibrato relevance flags keep non-vibrato
  blocks at their pre-change cost, and the whole-voice plan now accepts
  unmapped CCs as audio-exact no-ops (they have no engine effect).
- 2026-09-13 fix(v3): per-segment rdtsc profiling in RenderWholeVoiceSegment
  gated behind SVMS_WV_SEGMENT_PROFILE (default off). The six rdtsc per
  segment added by the OneShot-kernel telemetry cost ~+8% on note-only
  material and ~+25-35% on op-fragmented blocks (every CC/bend row-op splits
  timelines into more segments). Bench dispatch fixes alongside: pitch bends
  now route through ApplyChannelBendRatio like Driver::HandlePitchBend (the
  old inline rewrite bypassed the whole-voice bend-op hook, so bend-heavy
  bench numbers were fictional), and --cc-controller 1 emits nonzero values
  so the vibrato-gate A/B is honest. Measured @5000 voices chopped-notes:
  baseline 0.617 -> 0.570, +CC7 1.261 -> 0.816, mixed-events 4.156 -> 3.144;
  the CC1 vibrato gate (legacy sparse fallback) measures 5.253 = the 9.2x
  cliff the owner flagged.
- 2026-09-12 feat(v3): BASS shim surface extended to the full genuine
  BASSMIDI export list (40 names). BASS_MIDI_StreamEvent (singular — the
  API PFA-1.1.0viz imports) is implemented for real: translates via the
  verified type table, anchors at the consumption cursor, and round-trips
  through a new per-stream (channel,type)->param cache that
  BASS_MIDI_StreamGetEvent queries. FontFlags/SetVolume/GetVolume,
  StreamLoadSamples, StreamSetFilter, FontCompact/Unload are functional
  no-ops; file/URL/user-stream creation, event/mark/preset queries, and the
  BASS_MIDI_In* surface are graceful-failure stubs so statically-importing
  hosts load (file-based MIDI creation would need a parser — the BPFA/PGFA
  family uses StreamCreate + StreamEvents per the decompiles). Forwarder
  and both /EXPORT lists updated; probe gained singular-API tests.

- 2026-09-12 feat(v3): offline/standalone synth mirrors the realtime
  same-key note-on coalescing (SVMSNoteOnCollapse gate in the audio-frame
  domain — same fixed 20 ms window, same note_on_collapse.threshold config
  knob, same velocity stacking of collapsed hits, CC120/123 and reset clear
  the gate). Optional by construction: threshold < 2 (the default) spawns
  every note-on. Live-pump bench, 800k notes/s hammered on 4 keys: 0.29x
  realtime with coalescing off (6.4M launches, 12.8M steals) vs 2.45x with
  threshold 32 (208k launches, 411k steals) — 8.4x. This is the shape the
  owner's realtime path already handles via coalescing; prerender now
  matches it.
- 2026-09-12 feat(v3): offline telemetry gained dispatch_coalesced (struct
  128 -> 136 bytes, ABI asserts updated); the shim's profile/final log
  lines report it.

- 2026-09-12 fix(v3): BASS shim per-event log flood removed — StreamEvents
  logged a full file open/write/close line per call, and realtime pumps
  submit one call per MIDI event (the owner's %TEMP% log hit 160 MB /
  2.4M lines from a single Kiva session). Per-event lines are gone
  (failures only, rate-limited), GetData profile lines now every 1024th
  pull, BassLog is throttled to ~1 line/second with a forced variant for
  rare lifecycle lines (FontInit/StreamCreate/final stats), and the file
  self-truncates past 4 MB. Live-pump bench (one StreamEvents call per
  event, 10 ms pulls): 100k notes/s @ 200 poly = 4.6x realtime, 16k @ 88
  keys = 8.4x.
- 2026-09-12 fix(v3): realtime audio-callback census lines
  ([SVMS] sched/flow/pool/planRefuse, every 64 callbacks) are compile-time
  OFF by default (SVMS_AUDIO_CENSUS=1 to re-enable) — the only recurring
  debug-stream traffic in the winmm path, and the DebugView destabilizer in
  OmniMIDI's presence. One-time init/failure messages are unchanged.

- 2026-09-12 perf(v3): StandaloneSynth note-ons go through the production
  LaunchVoiceGroup transaction (setups built up front, batched victim
  selection, sibling slots feed layers) with steal batching enabled, instead
  of a per-region AllocateVoiceOrSteal + ConfigureVoice loop. Same victims
  and configuration; sustained workload 50x -> 80x realtime, note-on
  dispatch cheaper under pool pressure.
- 2026-09-12 feat(v3): offline telemetry extended (struct_size-gated, 64 ->
  128 bytes) with the render-path bitmask and the standalone synth's
  dispatch-phase cycle profile (note-on/off, region resolve, alloc,
  configure, control, render total); BASS shim logs it per stream (final)
  and every 64th pull. SVMS_BASS_THREADS env var pins the offline render
  thread count for parallel-scaling diagnostics (1 thread = 22.4Gcyc vs
  auto 3.9Gcyc wall on the chopped load — the whole-voice render scales).

- 2026-09-12 perf(v3): BASSMIDI offline sessions render through the
  production RenderBlock machinery. The native offline path dispatched each
  event individually and rendered a span between events — at Black MIDI
  densities a full RenderBlock call every few frames (0.08x realtime on a
  chopped-notes load). Events now convert to RenderEvents and hand the whole
  block to RenderBlock (whole-voice/dense/sparse + worker pool), via a
  production EventDispatcher wired into StandaloneSynth whose bend routing
  goes through VoiceManager::ApplyChannelBendRatio (pre-pass-aware).
  Throughput: 0.08x -> ~10.4x realtime on chopped-notes @16k note rate;
  sustained ~49x; probe peaks bit-identical to the old path.
- 2026-09-12 perf(v3): BASS shim render-ahead cache — GetData refills in
  max_block_frames engine chunks and serves pull-sized requests, so
  small-pull callers (CSCore ISampleSource reads) match big-pull throughput.
  Only streams proven batch (a TIME-anchored event seen, no positionless
  event) render ahead; realtime pumps keep exact cursor anchoring — a deep
  cache would anchor their positionless events in already-rendered audio
  (caught by the send-after-pull probe). GetPosition/IsActive/available now
  report the served cursor.
- 2026-09-12 feat(v3): StandaloneSynth gained RenderWithEvents (event-batched
  RenderBlock with correctnessMode=false, i.e. production decimation tiers)
  plus a static DispatchRenderEvent; offline sessions hold back events at
  frame_offset == frameCount and apply them after the block (outside tested
  RenderBlock dispatch territory).

- 2026-09-11 fix(v3): BASS shim GetData BASS_DATA_FLOAT corrected to
  0x40000000 (reflected Bass.Net) — the shim stripped 0x400, so Kiva's
  1 MB pulls arrived still flagged and were treated as ~1 GB requests,
  rendering frames far past the caller's buffer. This overrun is the
  strongest suspect for the owner's "trash audio" prerender report.
- 2026-09-11 fix(v3): BASSMIDI struct-event type table corrected against
  Bass.Net's BASSMIDIEvent reflection: REVERB=23/CHORUS=24 (16/17 are
  SOUNDOFF/RESET — the old table played SoundOff/Reset as reverb/chorus
  CCs). Added SOUNDOFF→CC120, RESET→CC121, NOTESOFF→CC123, BANK_LSB(70)→CC32,
  SOSTENUTO(76)→CC66, KEYPRES(71)→poly aftertouch (key in HIWORD). RPN
  (5/7/8), reverb/chorus sends, and per-note controllers stay skipped — the
  engine maps none of those CCs.
- 2026-09-11 fix(v3): BASS shim font surface: BASS_MIDI_FontInit keeps one
  slot per path (handles never reused, FontFree marks freed),
  BASS_MIDI_StreamSetFonts parses the real count-flag formats (0x1000000 =
  24-byte FONTEX — verified in Bass.Net's wrapper IL and the genuine
  BASSMIDI copy helper disassembly — else 12-byte FONT), and streams take
  the list's first valid font (BASSMIDI's earlier-entry-wins priority;
  single-font fallback = last FontInit). A post-create StreamSetFonts that
  changes the priority font rebuilds the untouched session.
- 2026-09-11 feat(v3): BASS shim honors BASS_ATTRIB_MIDI_VOICES
  (Kiva's RenderVoices): setting it at frame 0 rebuilds the offline session
  with that pool size; GetAttribute round-trips it and reports
  MIDI_VOICES_ACTIVE via session telemetry.
- 2026-09-11 perf(v3): BASSMIDI offline sessions unthrottled —
  render_threads = 0 (engine auto: hardware concurrency, cap 16), backend
  AUTO (was 1 SCALAR thread), pool 4096 (was 2048), limiter OFF (real
  BASSMIDI emits raw float and Kiva applies its own LoudMax; double
  limiting pumped). This is the shim's "struggles rendering when it can
  clearly give more" fix.
- 2026-09-11 fix(v3): BASS shim StreamEvents — same-frame events keep
  submission order (stable sort; the packed-message tiebreaker reordered
  RPN/data sequences), and mode's low 16 bits now act as the 1-based
  channel override on all three event paths.
- 2026-09-11 fix(v3): BASS ChannelGetData (nullptr, 0) answers the
  available-bytes query (bytes up to the 2 s tail) instead of erroring.
- 2026-09-11 test(v3): bass_chain_probe extended — two font slots, FONT +
  FONTEX SetFonts formats, bad-handle rejection, MIDI_VOICES rebuild
  round-trip, SOUNDOFF/RESET/KEYPRES struct acceptance, available query;
  probe pulls now pass the real 0x40000000 flag.

- 2026-09-09 fix(v3): VEH crash reporter scoped to faults inside our own
  module. Under Kiva + OmniMIDI KDMAPI, OmniMIDI's engine AVs inside
  USER32 wvsprintfA (its own debug formatting) and our first-chance VEH
  walked that foreign stack — destabilizing DebugView (which itself AVs on
  debug streams emitted in that state) and masking the real fault.
- 2026-09-09 fix(v3): BASS shim gained a file diagnostic channel —
  %TEMP%\svms_bass.log (FontInit/StreamCreate/StreamEvents/GetData/ENDED),
  independent of OutputDebugString so it survives the DebugView crash.
- 2026-09-09 fix(v3): BASS shim exports the BASS_FX surface (BASS_FXReset/
  Free/SetParameters/GetParameters/SetPriority/Version) as no-ops. OmniMIDI's
  KDMAPI loader binds its BuiltInEngine's BASS_FX imports to whatever module
  is loaded as bassmidi.dll (its own BASSMIDI build merges BASS_FX); a shim
  without those entry points killed KDMAPI process-wide whenever our
  bassmidi.dll was present alongside it.
- 2026-09-09 fix(v3): BASSMIDI event modes corrected against the real ABI
  (Bass.Net reflection): STRUCT=0, RAW=0x10000, SYNC=0x1000000,
  NORSTATUS=0x2000000, CANCEL=0x4000000, TIME=0x8000000 (the shim had
  SYNC/TIME/CANCEL values shifted). TIME positions by the stream BYTE
  position (RAW block header / BASS_MIDI_EVENT.pos); position-less events
  (RAW without TIME — Kiva's SendEventRaw plain 3-byte messages) anchor at
  the pull cursor. Probe verifies all three realtime shapes + drain.
- 2026-09-09 fix(v3): BASSMIDI flagless StreamEvents = BASS_MIDI_EVENTS_SYNC
  — events apply at the CURRENT pull cursor instead of tick 0. Kiva's
  realtime generator drains up to the event time and then sends flagless
  RAW blocks (RAW|NORSTATUS); anchoring them at the pull position makes the
  shim match the realtime contract (was: every note landed at frame 0 ->
  completely silent output). Probe: send-then-pull sounds, pull-then-send
  leaves the earlier window silent.
- 2026-09-09 fix(v3): BASSMIDI decode streams are FINITE like real BASS —
  GetData past the last event + 2 s tail returns -1/BASS_ERROR_ENDED (45),
  crossing pulls return partial data. Without termination, a broken caller
  length (Kiva Modded's generator wraps its ring count negative -> a
  ~4 GB DWORD length) rendered silence forever past the caller's pinned
  buffer: the Kiva AV in bass.dll. Verified by drain probe: full, full,
  partial, -1/45.
- 2026-09-09 fix(v3): BASS prerender pulls actually render — GetData pumps
  through max_block_frames-bounded chunks (NativeRenderOffline rejects
  frameCount > max_block_frames; the shim asked for unbounded pulls), the
  session requests 64k-frame blocks, FontInit accepts UTF-16 paths with or
  without the BASS_UNICODE flag (BASS.NET marshaling), and FontInit /
  StreamCreate failures LOG their path and native result code.
- 2026-09-09 fix(v3): BASSMIDI StreamCreate first parameter is the MIDI
  channel count, not output channels — Kiva's 16-channel request now passes
  and streams always render the stereo pair (was rejected with a mislabeled
  BUFLOST error code).
- 2026-09-09 fix(v3): BASSMIDI shim surfaces Kiva — added BASS_MIDI_FontLoad,
  BASS_ChannelFlags and BASS_GetVersion exports (host + forwarder), and
  corrected BASS_MIDI_StreamEvents to the real 4-arg bassmidi ABI
  (handle, mode, events, length) with TIME/tick/RAW/CANCEL mode parsing;
  render pulls now report BASS_ERROR_ENDED (45) past the event tail so
  prerender pumps terminate.
