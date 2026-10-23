// BassVstShim.cpp - the bass_vst.dll build.
//
// Real bass_vst.dll refuses to load next to the SVMS bass.dll (it asks bass.dll
// for an undocumented table at start-up and shows "Incorrect BASS.DLL version"),
// and KMC calls into it even when no VST is in use (BASS_VST_ChannelRemoveDSP
// when a conversion is cleaned up). This stand-in loads cleanly and reports
// "no plug-in" for everything: handles are 0, queries fail, and the calls that
// release or switch things off succeed. VST effects/instruments are not
// supported when KMC runs on the SVMS bass.dll. x64 only: the zero-argument
// signatures rely on caller-side argument cleanup.

#include <windows.h>

#if defined(_WIN64)
extern "C" {
__declspec(dllexport) DWORD WINAPI BASS_VST_ChannelCreate(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_ChannelFree(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_ChannelRemoveDSP(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_ChannelSetDSP(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_ChannelSetDSPEx(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_Dispatcher(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_EmbedEditor(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_GetBypass(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_GetChunk(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_GetInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_GetParam(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_GetParamCount(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_GetParamInfo(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_GetProgram(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_GetProgramCount(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_GetProgramName(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_GetProgramParam(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_ProcessEvent(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_ProcessEventRaw(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_Resume(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_SetBypass(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_SetCallback(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_SetChunk(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_SetLanguage(void) { return 1u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_SetParam(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_SetProgram(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_SetProgramName(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_SetProgramParam(void) { return 0u; }
__declspec(dllexport) DWORD WINAPI BASS_VST_SetScope(void) { return 0u; }
}  // extern "C"
#endif
