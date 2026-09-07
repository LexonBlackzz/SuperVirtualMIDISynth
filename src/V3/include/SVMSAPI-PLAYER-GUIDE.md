# SVMS-API integration guide for players

This guide is aimed at the author of a MIDI player — concretely, ziggy —
who I want to support the SuperVirtualMIDISynth API (`SVMSAPI.dll`) as a
first-class synth. It shows three levels of integration, from zero code to
the full native surface, and what each level buys.

Everything below needs exactly **one file**: `SVMSAPI.h` (single C/C++
header, no import library, no .lib, LoadLibrary-based). It works from any
toolchain that can call WinAPI.

---

## Level 0 — you may already be running on it

If your player loads `OmniMIDI.dll` by name from its own directory
(ziggy's `KdmapiLoader` does exactly this: `LoadLibrary("OmniMIDI.dll")`,
then `GetProcAddress` for the KDMAPI set), then placing SVMS's
`OmniMIDI.dll` alias next to the exe transparently reroutes every event
into the SVMS engine. No source change, no manifest, nothing. This is how
ziggy behaves today when the SVMS build is installed: the loader binds, and
playback runs on SVMS.

That works because SVMS deliberately exports the surfaces players already
probe: the full KDMAPI set (`IsKDMAPIAvailable`,
`InitializeKDMAPIStream`, `TerminateKDMAPIStream`, `ResetKDMAPIStream`,
`SendDirectData(NoBuf)`, `SendDirectLongData(NoBuf)`, …) **plus** the
telemetry trio your loader looks for — `GetVoiceCount`, `GetVoiceStatistics`
(a 12-byte struct: `[0] active, [4] free, [8] steals` — identical layout to
SnappySynth v2's), and `GetRenderingTime` (render time of the synth's own
audio path, in milliseconds).

If level 0 is all you want, you are done reading. The rest is about making
the support first-class instead of incidental.

## Level 1 — first-class client, ~15 lines

Drop `SVMSAPI.h` into the project, include it **once**:

```c
#include "SVMSAPI.h"
```

That single include binds the entire surface automatically — LoadLibrary,
GetProcAddress, null-safe wrappers — and degrades to neutral values when
the DLL is absent. Your synth menu then has a "SVMSAPI" entry backed by:

```c
/* open (when the user picks the synth) */
if (!IsKDMAPIAvailable() || !InitializeKDMAPIStream())
    return; /* synth unavailable — keep your other backends */

/* per event, from your existing submit path (same shape as today) */
SendDirectDataNoBuf(msg);        /* packed 0x00sskkvv */

/* telemetry, same slots your loader already probes */
DWORD voices            = SVMSAPI_GetVoiceCount();
SVMSAPI_VoiceStatistics stats;
SVMSAPI_GetVoiceStatistics(&stats);   /* .active_voices/.free_voices/.voice_steals */
float render_ms         = SVMSAPI_GetRenderingTime();

/* close */
ResetKDMAPIStream();
TerminateKDMAPIStream();
```

Notes:

- The wrappers are plain C, `WINAPI`, safe from any thread, and never
  block: the synth is fire-now, and the pacing/ordering work (timestamps,
  throttling, controller collapse, optional velocity shedding when the
  synth falls behind) happens inside SVMSAPI, not in your code.
- `SVMSAPI_MODULE_NAME` can be defined before the include to point the
  same header at a different KDMAPI synth
  (`L"OmniMIDI\\OmniMIDI.dll"`) — one integration, every synth.
- Your existing loader can stay as a fallback; probing `SVMSAPI.dll` first
  and the legacy `OmniMIDI.dll` path second is the recommended order.
- Honest note for v2.5: your current UI does not display synth render time
  yet (the `GetRenderingTime` slot is bound by your loader but never
  polled). The value is live on the SVMS side — the moment a CPU readout
  is added, it lights up with no synth-side change.

## Level 2 — the native surface (what SVMS itself uses)

The KDMAPI set is the compatibility layer. The native table
(`SVMS_GetInterface`, ABI 1, in `src/V3/include/svmsapi.h`) is what
SVMS's own player uses, and it adds:

- **Timed batches with four timestamp domains** — immediate, absolute
  output frames, QPC ticks, monotonic nanoseconds, mixed freely in one
  batch without quantization. Your player's current backlog-skip logic
  becomes unnecessary: submit the song's events with their true timestamps
  and the engine schedules them exactly.
- **Telemetry V2** — the complete engine census in one struct: lateness,
  shedding, CC-collapse counts, scheduler backlog, which renderer each
  block used, callback budget percentiles (p95/p99/p99.9).
- **Runtime commands** — the same live controls the SVMS configurator
  uses: steal policy, per-key voice cap, retire floor, CC collapse, block
  timing, ghost budget, thread affinity. Expose them in your UI and the
  user tunes the synth from inside your player.
- Offline/analysis sessions for rendering without an audio device.

The integration is still runtime-only (`SVMS_GetInterface` + one struct);
`svms_player.exe` in the SVMS repository is a complete reference client
(`Api::Load` — table negotiation with required-slot validation, event
submission, telemetry). Full documentation: `src/V3/include/README.md`.

## Why bother, when the drop-in already works?

The drop-in makes ziggy *speak KDMAPI to SVMS*. First-class integration
makes ziggy speak SVMS: exact-frame scheduling instead of fire-now pacing,
the engine's census visible in your overlay, live tuning of the engine's
synthesis knobs, and — because SVMSAPI.dll internally routes to any
backend (SVMS engine, KDMAPI synths, WinMM devices, or any auto-detected
synth DLL) — your player instantly supports *every* synth that speaks any
of those surfaces, with one code path.
