# Changelog

## 0.11.0 (2026-10-17) — 131 commits since v3-0.10 (2026-09-09)

A new Configurator, a single renderer, and a lot of Black MIDI performance work.

### Configurator, rewritten

- **New look and shell.** Every page was rebuilt: Home, Performance, Sound,
  Effects, Output, MIDI, Render to file, Record, Diagnostics and Settings.
  Geist fonts are embedded, controls spring and ease (Motion: Full / Calm /
  Off), and one settings table drives search, the restart marks (↻), the
  save bar and the sidebar badges.
- **Home**: a render-load gauge that changes colour by tier and catches fire
  past 500 % (can be turned off), plus voice, MIDI and output tiles.
- **Performance**: Accurate / Balanced / Extreme profile cards with Custom
  detection, voice limit with a typed Custom field, shortcuts grouped as
  Free / Costs a little sound / Costs accuracy or timing, an Engine internals
  section, and a render-threads slider.
- **Sound**: one SoundFont list (drop .sf2/.sfz on the window, drag to
  reorder), a velocity curve with a live response graph, voice character with
  a description per mode, tuning, and bank/preset routes.
- **Settings**: UI size (75–175 %), themed colour picker, a welcome tour on
  first launch, and the Rack look kept as an alternative theme.
- **Render to file** now matches the live sound: reverb, velocity curve,
  pan law, interpolation and render backend come from the shared config, and
  the tab passes its unsaved settings to the renderer.
- Controls a driver can't change live are named as such instead of retried.

### Renderer / performance

- **One renderer.** The dense planner and the span renderer are gone. The
  whole-voice renderer is the only path and a plan can no longer refuse a
  block. It now also covers vibrato, phase rotation, note merging and
  decimation.
- **Voice decimation** (opt-in): above 10k/25k/50k/150k active voices the
  quietest voices stop mixing. It can rank by actual level instead of
  velocity, and optionally cull tails that stay muted for 5 blocks.
- **Eight-voices-per-vector filter**: the SF2 low-pass was about 75 % of a
  live voice; the new kernel is bit-identical per voice.
- **AVX2**: one-shot kernels (pianos, plucks) went from scalar fallback to
  vectorized (prerender 10.6× → 21.3× realtime), exact kernel trims, an
  opt-in fast chunk math mode, and exact phase rotation on the AVX2 chunk
  kernels.
- **Event pipeline**: opt-in single-producer ingest ring, batched ingress for
  mixed events, bend collapse, lazy queue storage, and workers no longer share
  hot counter cache lines.
- **Plan loop** trims: O(1) release-op drop, slimmer ghost copies,
  same-frame note batching, segment batching.
- **Key-sharded engine** (opt-in, `synth.shards`): the voice pool and event
  stream split across N independent cores. The voice limit is a total divided
  across shards, and the cap scales to 524,288 × shards.

### New options (all off unless noted)

Exact note merge · note-on re-excite (keeps the rhythm of merged repeats) ·
minimum note length · steal reject · bend collapse · fast chunk math ·
decimation by level · cull muted tails · per-MIDI-channel limiter (16
zero-latency buses).

**Changed defaults:** the retire floor is now −55 dB and decimation (when on)
ranks by level. A saved config keeps its own values. Both were chosen in
listening tests.

### Instruments and tuning

- **SFZ instruments**, including `#include`, alongside SF2, with filtering for
  both formats.
- **256-key** and **31-EDO** support.
- Retire floor now actually governs real SF2 release tails (it did nothing for
  them before).

### Linux

- Linux **configurator** (SDL2 + OpenGL3) driving the driver over a new
  **Control API v1** (self-describing NDJSON over a Unix socket).
- The Linux realtime loop renders each block in one batched call, falls back
  to `gm.sf2` when no SoundFont is configured, and the config module is
  portable (XDG paths).

### Integrations

- **BASSMIDI shim** now serves Kiva: full BASS/BASSMIDI export surface, real
  event-type table, partial-read decode streams, and offline sessions through
  the production renderer (0.08× → 10× realtime on chopped-notes).
- `midiOutGetID`/`midiInGetID` exported so Domino loads our winmm; a
  nested-backend guard stops an SVMS plugin inside another host from looping.
- Per-callback trace ring on the native API; Record and Render pages in the
  configurator.

### Fixes

- Releasing-voice counter underflow when a voice released and retired in one
  block; loop-wrap overflow into short spans; per-voice phase rotation made
  exactly analytic; the 131072-event block cap only limits late events;
  per-channel limiter clicks and a live-flush bug.
- Configurator: velocity knobs overflowing at narrow widths, overlapping
  render-time text, tile flicker at fractional scales, and the live voice
  limit giving every shard the full count.

### Build / tooling

- The engine compiles once for winmm.dll and SVMSAPI.dll, with a precompiled
  driver header and CI compiler caches. SVMSDriver.cpp was split by area.

## 0.10.0 (2026-09-09) — 127 commits since v3-0.8 (2026-08-27)

0.9 skipped. Not 1.0 — the software isn't finished and never will be.

### Renderer / performance

- **Per-voice whole-block renderer** — the largest change of the cycle. Note
  blocks render as one exact-frame plan per voice: launches dispatch through
  the production path, releases defer to block end, displaced victims render
  in-band as ghosts from their exact pre-steal state. Then extended to carry
  the lifecycle controllers (64/66/120/123) and the row-mutating CCs
  (pitch bend, 0/32/6/38, 7/10/11, 96/97, 121, program change, rhythm part,
  master SysEx) — CC-bearing Black MIDI material went from ~3.7 to ~0.5
  cycles per voice-sample (6–7×).
- **i16 sample store** — half the cache footprint, exact ×(1/32768) on-load
  conversion.
- **AVX2 kernels**: transient-loop envelope kernel, voice-batched short-span
  kernels (8 voices/lane), parallel transient/release spans on the worker
  pool, wrap-free render chunks, region-grouped voice order, next-chunk
  sample prefetch.
- **Dense planner**: touched/untouched tiles, indexed event marking, adaptive
  stand-down gate, touched-voice buckets through class kernels,
  grown-capacity storage, unified gate.
- **Event pipeline**: power-of-two-masked MPSC ring with consumer prefetch,
  guarded compiler wake, same-key note-off compaction, bounded per-block
  admission, exclusive-block direct dispatch, invariant-TSC producer
  timestamps (producer ≈ 215M events/s, compiler ≈ 93M events/s).
- **Steal machinery**: batched candidate selection for launch groups,
  volatile heap rebuilt once per block, prefetching. New experimental
  Fast-cursor (O(1)) and Scan (SIMD window) policies — Quality remains the
  recommended one.
- Configurable voice retire floor (dB slider, live).

### Phase rotation

- Rotation moved from post-mix to per-voice at note dispatch.
- Modes 1/2/4 (Analytic/Sweep/Random) now use the **exact analytic Hilbert
  pair**: an i16 companion store precomputed per soundfont sample at load
  (FFT-based, matching the reference offline synth), `y = x·cosθ − x̂·sinθ`.
  Allpass splitter remains as fallback when the bundle carries no pair;
  Coherent mode 0 stays bit-identical.

### Audio backends

- **ASIO output** (optional build): driver picker in the configurator,
  reset/format-change handling with live adoption, silent-output fixes
  (proper deinterleave, float64/int24), reopen backoff, on-the-fly buffer
  size stress test.
- Configurator: WASAPI/ASIO backend selector.

### SVMS-API / integrations

- **SVMSAPI.dll is a universal synth router**: its event pipeline terminates
  in a pluggable sink — in-process engine (default), external SVMS-API DLL,
  external KDMAPI DLL, WinMM MIDI-out device, or auto-detect
  (SVMSBackend_GetInterface → KDMAPI exports → midiOut*).
- **SVMSAPI.h single-header integration** (drag-n-drop like OmniMIDI.h):
  client binders + the third-party backend contract; native API runtime
  commands and Telemetry v2; full docs in `src/V3/include/README.md`.
- Standalone SVMSAPI.dll target with its own export surface; KDMAPI facade
  aligned with the canonical OmniMIDI contract.
- **BASS/BASSMIDI prerender shim**: bass.dll (full alias + BASS/BASSMIDI
  exports) + bassmidi.dll forwarder; decode streams are offline sessions —
  real-time vs offline bisecting for playback artifacts.
- **svms_player**: synth discovery, KDMAPI/WinMM backends, ImGui GUI,
  runtime synth switching, in-GUI song open, NPS + MIDI-polyphony graphs,
  persistent song mapping with resume.

### Opt-in features (all default off)

Per-key voice cap · hybrid P/E-core thread affinity · compiler-side CC
collapse · block-granular dispatch ("OmniMIDI mode") · ghost budget ·
large-page pool backing · unbounded render (no recovery jump, no admission
cap) · Kiva-rule lateness shedding for external backends.

### GPU (experimental)

- D3D11 compute synthesis backend (proof spike −132.8 dB vs CPU reference),
  integrated into the offline and standalone renderers.

### Fixes

- Whole-voice renderer crash class (uninitialized retire counts — the live
  CC120/CC123 freeze), stale deferred-release overflow hang, span-worker
  class race, dense wrap guard, rotation stuck voices, steal-policy
  live-switch race (parks to the audio thread), i16 load normalization,
  configured render backend being dead code.
- VEH crash reporter with symbolized stacks + DLL self-pin against
  mid-render FreeLibrary; hostile-exit test suite.
- Linux/MinGW builds repaired: POSIX worker pool, GCC compile guards.

### Tooling / tests

- `svms_v3_midi_song` real-SMF playback gate (offline silent default,
  optional audible realtime) with hang watchdog and dispatch-phase cycle
  profiler; per-class voice census; long-span AVX2 differential;
  live-signature bench workload (`chopped-notes`); ASIO reopen stress test;
  ref numbers refreshed in the reference docs.

## Older versions

v3-0.8 and earlier predate this changelog; see the git log.
