# Changelog

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
