# SVMSAPI — the synth-agnostic MIDI interface

SVMSAPI speaks three client surfaces and loads three kinds of synth
backends. Any player can drive any synth through it, and any synth can plug
into it, using nothing but `LoadLibrary`/`GetProcAddress` — or the
single-header integration in `src/V3/SVMSAPI.h`.

| Client surface | What it is | Who uses it |
|---|---|---|
| **SVMS native API v1** (`svmsapi.h`, below) | A versioned function table with timed batches, offline sessions, telemetry and config access | Players that want the full pipeline: exact frames, lossless queues, cancellation |
| **KDMAPI facade** | The OmniMIDI-compatible export set (`InitializeKDMAPIStream`, `SendDirectDataNoBuf`, …) | Players written against OmniMIDI.h/KDMAPI — drop-in |
| **WinMM shim** | The full `midiOut*`/`midiIn*` surface exported by `winmm.dll` (and the `SVMS.dll` / `OmniMIDI.dll` / `SnappySynth.dll` aliases) | Any application that plays MIDI, with zero source changes |

| Backend (synth side) | How it plugs in | Selected by |
|---|---|---|
| **SVMS engine** | Built into SVMSAPI.dll | `api.backend = 0` (default) |
| **SVMS-API backend** | DLL exporting `SVMSBackend_GetInterface` (contract at the bottom of `src/V3/SVMSAPI.h`) | `api.backend = 1`, or auto-detect |
| **KDMAPI synth** | DLL exporting the KDMAPI set (OmniMIDI, …) | `api.backend = 2`, or auto-detect |
| **WinMM synth** | A MIDI-out device on the system winmm, or a WinMM-replacement DLL | `api.backend = 3`, or auto-detect |
| **Any of the above** | One DLL, probed for whatever surface it answers with | `api.backend = 4` (auto-detect) |

The router (SVMSAPI.dll / winmm.dll) owns the event pipeline — ordering,
pacing to playback time, CC collapse, optional velocity shedding — and the
backend owns synthesis and audio output. External backends therefore never
see the pipeline cost, and slow backends never stall the caller.

---

# SVMS native API v1

`svmsapi.h` is the stable C interface for applications that want to talk to
SuperVirtualMIDISynth directly. Load `SVMSAPI.dll`, resolve the single permanent
`SVMS_GetInterface` export, request ABI 1, and use only capabilities and
function pointers returned in that table.

```c
#include "svmsapi.h"
#include <windows.h>

typedef SVMS_Result (SVMS_CALL *GetInterfaceFn)(
    uint32_t, uint32_t, SVMS_Interface*);

HMODULE dll = LoadLibraryW(L"SVMSAPI.dll");
GetInterfaceFn get_interface = (GetInterfaceFn)(void*)
    GetProcAddress(dll, "SVMS_GetInterface");

SVMS_Interface api = {0};
if (!get_interface ||
    get_interface(SVMS_ABI_VERSION_1, sizeof(api), &api) != SVMS_RESULT_OK) {
    /* Runtime missing or too old. */
}

SVMS_SessionConfig config = {0};
config.struct_size = sizeof(config);
config.struct_version = SVMS_STRUCT_VERSION_1;

SVMS_Session session = 0;
if (api.create_session(&config, &session) == SVMS_RESULT_OK) {
    api.send_short(session, 0x00643C90u); /* C4, velocity 100 */
    api.send_short(session, 0x00003C80u); /* C4 off */
    api.destroy_session(session);
}
```

For exact scheduling, call `get_runtime_clock` and submit absolute QPC ticks
through `send_short_at_qpc` or `send_short_batch`. Batching does not quantize
timestamps: every event retains its own tick, and equal-tick events retain
array/ingress order.

Newer ABI-1 runtimes append an optional exact-timing/control tail to
`SVMS_Interface`. Check `api.struct_size`, the relevant capability bit, and the
function pointer before using it. This preserves binaries built against the
original, shorter ABI-1 table.

`send_timed_short_batch` accepts one timestamp domain per event:

- `SVMS_TIMESTAMP_IMMEDIATE` for the next writable frame;
- `SVMS_TIMESTAMP_OUTPUT_FRAME` for an absolute frame from `get_output_clock`;
- `SVMS_TIMESTAMP_QPC` for Windows QPC ticks;
- `SVMS_TIMESTAMP_MONOTONIC_NS` for the portable monotonic clock returned by
  `get_monotonic_clock`.

The runtime converts each record independently. A mixed batch may therefore
contain immediate, output-frame, and wall-clock events without quantizing them
to one boundary. Windows also advertises optional queue mode/query controls,
UTF-8 SoundFont reload, and ordered panic when those function pointers are
available.

On Linux, load `libsvmsapi.so`. The ABI is unchanged, but a runtime advertising
`SVMS_CAP_EXACT_MONOTONIC_NS` returns a 1 GHz monotonic-nanosecond clock from
`get_runtime_clock`; the legacy `qpc` field/function spelling is retained only
to keep ABI 1 identical across platforms.

Linux accepts immediate, absolute-output-frame, and monotonic-nanosecond records
through `send_timed_short_batch`. It rejects the Windows-only QPC timestamp
domain. Queue pressure can be queried, but the current Linux ingress remains
lossless and does not advertise runtime queue-mode control.

All extensible structures carry `struct_size` and `struct_version`. Initialize
reserved fields to zero. Do not copy internal C++ types or RuntimeLink shared
memory layouts into applications.

`SVMSAPI.dll` is a dedicated build of the same engine sources as `winmm.dll`,
with its own export surface: the single `SVMS_GetInterface` bootstrap plus the
KDMAPI facade. It does not export the WinMM shim functions. `winmm.dll` (and
its `SVMS.dll`, `OmniMIDI.dll`, and `SnappySynth.dll` aliases) remain the full
drop-in shim runtime. WinMM and native-API clients therefore load different
modules by design; the engine implementation is shared at the source level and
both export surfaces are covered by the ctest suite. Note that loading two SVMS
modules in one process creates two independent engine instances.

Handles returned by the original `create_session` function provide
reference-counted ownership of that single process engine. They are not
independent synth instances with separate MIDI state or SoundFonts. Isolated
caller-driven sessions use the capability and functions below; independent
native real-time audio sessions remain future work.

## Isolated offline and analysis sessions

Runtimes advertising `SVMS_CAP_ISOLATED_OFFLINE_SESSIONS` can create independent
caller-driven synths through `create_offline_session`. Each has its own
SoundFont, channel state, voice pool, renderer, limiter, and absolute output
frame. These sessions never open or share an OS audio device.

Fill `SVMS_OfflineSessionConfig`, pass a UTF-8 SoundFont path, then call
`render_offline` with planar caller-owned float buffers. Each
`SVMS_OfflineEvent.frame_offset` is exact within that call; offsets must be
nondecreasing and equal-frame events retain array order. An event at
`frame_offset == frame_count` changes state exactly at the boundary for the next
call.

`SVMS_SESSION_SILENT_ANALYSIS` runs the same synthesis and lifecycle state but
allows null output buffers. Scratch storage is allocated at session creation up
to `max_block_frames`, so rendering does not grow buffers. Use
`get_offline_telemetry` for position, event, voice, and stealing counters, and
destroy either kind with the original `destroy_session` function.

## Configuration access

Windows runtimes advertising `SVMS_CAP_CONFIG_JSON` expose the selected V3
configuration through `get_config_json` and `get_config_path_utf8`. Both use a
two-call buffer protocol: call with a null buffer to receive the required byte
count (including the terminator), allocate it, then call again.

`patch_config_json` accepts a UTF-8 JSON Merge Patch. It validates recognized
values, preserves untouched and unknown fields, serializes with the same
cross-process mutex as first-run creation, and atomically replaces the file.
Malformed documents, unsupported schemas, schema changes, and invalid known
values are rejected without modifying the original. Persisted changes require
an engine restart; this function does not silently live-reconfigure playback.

## Submission cancellation and SysEx ownership

Runtimes advertising `SVMS_CAP_CANCELLABLE_SUBMISSION` expose
`cancel_session_submissions`. It is a permanent submission fence for that
real-time session token: blocked lossless producers are woken, pending API calls
return `SVMS_RESULT_CANCELLED`, and later short, batch, and SysEx submissions
are rejected the same way. Telemetry, reset/panic, and destruction remain
available. Create a new session to resume submission.

The runtime consumes and translates the complete SysEx byte array before
`send_system_exclusive` returns. It never retains the caller's pointer, so the
caller may immediately reuse or release that buffer. The generated ordered
engine events may remain queued after the source bytes have been released.

---

# KDMAPI facade reference

`SVMSAPI.dll` (and the `OmniMIDI.dll` alias) export the OmniMIDI KDMAPI
surface. All functions are `WINAPI`. `src/V3/SVMSAPI.h` binds this whole set
automatically; KDMAPI.md (repository root) keeps the original OmniMIDI
documentation this table summarizes.

| Export | Signature | Behavior |
|---|---|---|
| `ReturnKDMAPIVer` | `BOOL (LPDWORD major, LPDWORD minor, LPDWORD build, LPDWORD revision)` | Reports the emulated KDMAPI version (4.1). |
| `IsKDMAPIAvailable` | `BOOL (void)` | Always TRUE — loading the module is the availability check. |
| `InitializeKDMAPIStream` | `BOOL (void)` | Opens the engine (or starts routing to the selected backend). Returns FALSE on failure. |
| `TerminateKDMAPIStream` | `BOOL (void)` | Closes the stream; releases the driver when the last front-end disconnects. |
| `ResetKDMAPIStream` | `VOID (void)` | Hard all-notes-off: voices stop, channel controllers reset. |
| `SendDirectData` | `VOID (DWORD msg)` | Queues one packed short message `0x00sskkvv` through the lossless pipeline. |
| `SendDirectDataNoBuf` | `VOID (DWORD msg)` | Same contract; the no-buffer spelling submits without an intermediate batch. |
| `SendCustomEvent` | `BOOL (DWORD eventtype, DWORD chan, DWORD param)` | BASSMIDI-style event triple. Mapped types are translated; unsupported types return FALSE (never garbage). |
| `SendDirectLongData` | `UINT (MIDIHDR*, UINT)` | Submits a SysEx buffer prepared by `PrepareLongData`. |
| `SendDirectLongDataNoBuf` | `UINT (LPSTR data, DWORD size)` | Submits raw SysEx bytes; consumed before returning. |
| `PrepareLongData` / `UnprepareLongData` | `UINT (MIDIHDR*, UINT)` | WinMM-compatible header lifecycle (flag bookkeeping only). |
| `DriverSettings` | `BOOL (DWORD setting, DWORD mode, LPVOID value, UINT cbValue)` | OM_GET answers from live engine state; unsupported ids return FALSE. |
| `GetDriverDebugInfo` | `void* (void)` | Opaque pointer to implementation-specific debug info (layouts differ between synths). |
| `LoadCustomSoundFontsList` | `VOID (LPWSTR)` | Accepted; SVMS loads SoundFonts through its own configuration. |
| `timeGetTime64` | `DWORD64 (void)` | 64-bit millisecond clock (no 49-day wrap). |

Short messages use the packed WinMM layout: `0x00sskkvv` — status byte in the
low byte (event type + channel), then data1, data2. Batches of direct
messages keep submission order; the pipeline preserves it into the engine.

The facade also exports the telemetry trio players probe next to KDMAPI
(ziggy-style loaders): `GetVoiceCount()` (`DWORD`), `GetVoiceStatistics()`
(12-byte struct: `[0]` active, `[4]` free, `[8]` steals — SnappySynth v2
layout), and `GetRenderingTime()` (synth render time in milliseconds,
refreshed every audio callback). The single header `src/V3/SVMSAPI.h`
binds all of it; a player integration walkthrough lives in
`SVMSAPI-PLAYER-GUIDE.md` (repository root).

---

# Finding synths: the discovery rules

## Client side (players)

`SVMSAPI.h` binds exactly one module. Resolution order:

1. `SVMSAPI_MODULE_NAME` if defined at include time (e.g.
   `L"OmniMIDI\\OmniMIDI.dll"` to drive OmniMIDI instead);
2. otherwise `SVMSAPI.dll`, from the application directory first, then the
   normal DLL search path.

If `IsKDMAPIAvailable()` returns FALSE after `SVMSAPI_Load()`, no compatible
synth module was found. `svms_player.exe` (the reference client, see below)
implements the same order for all three surfaces and additionally probes a
drop-in `winmm.dll`/`OmniMIDI.dll` placed next to the executable before
falling back to the system module — the exact scenario an end user gets when
they copy a synth next to a player.

## Host side (the router inside SVMSAPI.dll / winmm.dll)

`api.backend` picks the sink, applied at driver initialization:

- **0 — SVMS engine.** In-process; the default and unchanged behavior.
- **1 — SVMS-API DLL.** Loads `api.backend_dll`, requires the
  `SVMSBackend_GetInterface` export, requests the full function table
  (version + every core pointer validated before use), and calls
  `initialize` with the host's sample rate/buffer size.
- **2 — KDMAPI DLL.** Loads `api.backend_dll` and requires
  `InitializeKDMAPIStream` + `TerminateKDMAPIStream` + `ResetKDMAPIStream`
  + `SendDirectData` or `SendDirectDataNoBuf`. Calls `InitializeKDMAPIStream`
  once at load.
- **3 — WinMM device.** Opens MIDI-out device `api.winmm_device` through the
  *genuine system* winmm (resolved by absolute path — a router built as
  winmm.dll must never call its own exports). Devices are enumerated by
  `midiOutGetNumDevs`; index 0 is typically the Microsoft GS Wavetable Synth.
- **4 — Auto-detect DLL.** Loads `api.backend_dll` once and probes, in order:
  1. `SVMSBackend_GetInterface` → SVMS-API backend;
  2. the KDMAPI export set → KDMAPI backend;
  3. `midiOutGetNumDevs` + `midiOutOpen` + `midiOutShortMsg` → treated as a
     WinMM-replacement DLL (device `api.winmm_device` inside that module);
  4. none matched → refuse and fall back to the SVMS engine with a log line.

This is what makes "any synth that responds to any of the surfaces" work:
drop OmniMIDI.dll, a third-party winmm replacement, or your own SVMS-API
backend in, point `api.backend_dll` at it, select auto-detect. Whatever it
answers with, that is the synth.

A failed load of any kind always falls back to the in-process engine — the
router never renders silence because a backend was missing.

## Event delivery to backends

The router forwards admitted events as packed short messages in ingress
order, already throttled to playback time. Note-offs and controller
lifecycle events are never dropped (CC collapse and the optional
`PriorityVelocity` shedding happen upstream and obey the same rules for
every backend). The v1 backend contract does not deliver SysEx or
master-level controls (master volume/tune, rhythm part); resets become the
backend's hard reset.

---

# Engine live controls and telemetry (ABI-1 tail)

Runtimes advertising `SVMS_CAP_RUNTIME_COMMANDS` expose `send_runtime_command`
(non-XP builds). It drives the *same live-control surface the SVMS
configurator uses* — one call, one command:

```c
char text[256];
api.send_runtime_command(session, SVMS_COMMAND_SET_PER_KEY_VOICE_CAP, 4u,
                         text, sizeof(text));   /* SVMS_RESULT_OK */
```

Commands mirror the runtime-link ids: `SVMS_COMMAND_SET_STEAL_POLICY`,
`SET_PER_KEY_VOICE_CAP`, `SET_VOICE_RETIRE_FLOOR`, `SET_CC_COLLAPSE`,
`SET_BLOCK_TIMING`, `SET_GHOST_BUDGET`, `SET_NOTE_ON_COLLAPSE`,
`SET_PHASE_ROTATION`, `SET_THREAD_AFFINITY_MODE`, `REQUEST_RESTART`, and
`PING`. `param` carries the command argument (a policy index, a voice count,
raw IEEE-754 bits for the retire floor, 0/1 for toggles). The optional text
buffer receives a truncated human-readable outcome; pass NULL/0 to skip it.
Invalid arguments return `SVMS_RESULT_INVALID_ARGUMENT` without touching
engine state. New commands are added as new ids — callers must treat
unknown ids as `SVMS_RESULT_INVALID_ARGUMENT`, not as errors to retry.

Runtimes advertising `SVMS_CAP_TELEMETRY_V2` expose `get_telemetry_v2`, the
complete engine census: everything in `SVMS_TelemetryV1` plus lateness
counters (`late_events`, `late_clamped_events`, max lateness, block pileup),
shedding (`shed_note_ons`), compiler CC collapse drops, fence-suppressed
note-ons, scheduler backlog (`scheduled_events`), the render path the last
block used (`render_paths`: bit0 whole-voice, bit1 dense, bit2 sparse,
bit8 vibrato-forced legacy), the plan-refusal reason, the exact-frame scalar
span fallback totals, and callback budget statistics (p95/p99/p99.9 percent,
over-budget callback counts). This is the same data the driver's DebugView
census prints, in one struct.

# The reference client

`svms_player.exe` (`src/V3/SVMSPlayer.cpp`, built by `player_build.bat`)
is the dogfood client for all of this: it drives the
native table (`SVMS_GetInterface` with required-slot validation), a KDMAPI
synth, or a WinMM device, chosen at startup, with the same GetProcAddress
resolution an external integrator would use — including the drop-in
app-directory module shadowing the system one. Reading its `Api::Load` and
`LegacySink::Load` is the fastest way to see the discovery rules above
implemented end to end.

---

# SVMS-API backend contract v1

A synthesizer that implements this interface ships as a DLL exporting one
symbol, `SVMSBackend_GetInterface`. The full contract — struct layouts,
calling rules, and the capability flags — lives at the bottom of
`src/V3/SVMSAPI.h`, so synth authors need exactly one file. Summary:

- The host presets `out->struct_size` to `sizeof(SVMSBackendInterface)`; the
  backend fills the table and returns 0, or non-zero to refuse.
- `initialize(user, params)` runs once before any send and returns 0 on
  success; `reset` is a hard all-notes-off; `shutdown` after the final send.
- `send_short(user, msg)` consumes packed `0x00sskkvv` messages and must not
  block — the host may deliver hundreds of thousands per second.
- `send_short_batch` is optional; advertise `SVMSBACKEND_CAP_BATCH` and the
  host will use it for ordered best-effort batches.
- SysEx and master-level controls are not delivered in v1.
