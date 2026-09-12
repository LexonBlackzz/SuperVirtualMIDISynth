# SVMS V3 — running changelog

Low-key running log, updated with every landed change. The root
`CHANGELOG.md` carries the polished per-version entries I publish when I cut
a release; this file is where changes land first, raw, under "Unreleased",
until I move them into a release section.

Format: newest first, one bullet per landed change, matching the commit's
`type(v3): one-liner` style plus a line of context where it helps.

## Unreleased

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
