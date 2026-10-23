// BassWasapiShim.cpp - the basswasapi.dll stand-in.
//
// Real basswasapi.dll cannot run next to the SVMS bass.dll (it binds to an
// undocumented export table of the genuine bass.dll and faults in
// BASS_WASAPI_GetVersion). KMC only touches it for realtime monitoring and in
// its cleanup path (BASS_WASAPI_Stop), neither of which a prerender needs, so
// everything reports "no device": Init fails, handles/queries are 0, and the
// release calls succeed. x64 only: the zero-argument signatures rely on
// caller-side argument cleanup.

#include <windows.h>

#if defined(_WIN64)
extern "C" {
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_CheckFormat(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_Free(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetCPU(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetData(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetDevice(void) { return 0xffffffffu; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetDeviceInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetDeviceLevel(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetLevel(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetLevelEx(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetMute(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetVersion(void) { return 0x02040100u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_GetVolume(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_Init(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_IsStarted(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_Lock(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_PutData(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_SetDevice(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_SetMute(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_SetNotify(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_SetVolume(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_Start(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_WASAPI_Stop(void) { return 1u; }
}  // extern "C"
#endif
