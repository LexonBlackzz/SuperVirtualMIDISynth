# SVMS V3 — running changelog

Low-key running log, updated with every landed change. The root
`CHANGELOG.md` carries the polished per-version entries I publish when I cut
a release; this file is where changes land first, raw, under "Unreleased",
until I move them into a release section.

Format: newest first, one bullet per landed change, matching the commit's
`type(v3): one-liner` style plus a line of context where it helps.

## Unreleased

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
