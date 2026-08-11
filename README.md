# SuperVirtualMIDISynth

This is a MIDI synth, as you could probably tell from the name.

SVMS V3 is still a work in progress, so I won't publish any prebuilt DLLs yet. Stuff may be broken or change at any time.

You can still build it from source if you want to try it.

## Build Requirements

- Windows
- Visual Studio 2022 (or 2026)
- CMake 4
- Ninja

Run `build_v3.bat` to build the x64 version. The DLL will be placed in `build\V3\bin\winmm.dll`.

There are also separate scripts for x86 and Windows XP builds. I do not guarantee success of those working, as I was only able to briefly test an XP build in VMWare, and it was functional, YMMV.
