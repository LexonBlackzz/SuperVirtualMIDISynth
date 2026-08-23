# SVMS native API v1

`svmsapi.h` is the stable C interface for applications that want to talk to
SuperVirtualMIDISynth directly. Load `SVMS.dll`, resolve the single permanent
`SVMS_GetInterface` export, request ABI 1, and use only capabilities and
function pointers returned in that table.

```c
#include "svmsapi.h"
#include <windows.h>

typedef SVMS_Result (SVMS_CALL *GetInterfaceFn)(
    uint32_t, uint32_t, SVMS_Interface*);

HMODULE dll = LoadLibraryW(L"SVMS.dll");
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

All extensible structures carry `struct_size` and `struct_version`. Initialize
reserved fields to zero. Do not copy internal C++ types or RuntimeLink shared
memory layouts into applications.

`winmm.dll`, `SVMS.dll`, `OmniMIDI.dll`, and `SnappySynth.dll` are byte-identical
names for one runtime in a given build. This is deliberate: WinMM, native API,
KDMAPI, and Ziggy integrations share one scheduler, one synthesis engine, and
one ownership model.
