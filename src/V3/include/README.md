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

`winmm.dll`, `SVMSAPI.dll`, `SVMS.dll`, `OmniMIDI.dll`, and
`SnappySynth.dll` are byte-identical names for one runtime in a given build.
`SVMSAPI.dll` is canonical; `SVMS.dll` is retained as a compatibility spelling.
This is deliberate: WinMM, native API, KDMAPI, and Ziggy integrations share one
scheduler, one synthesis engine, and one ownership model.

ABI-1 session handles currently provide reference-counted ownership of that
single process engine. They are not yet independent synth instances with
separate MIDI state or SoundFonts; applications must check future capabilities
before assuming isolated real-time, offline, or analysis sessions.
