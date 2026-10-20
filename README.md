# SuperVirtualMIDISynth

A SoundFont MIDI synthesizer built for Black MIDI: sample-accurate event timing,
dense note streams, and voice pools from a few hundred up to 524,288 voices.

On Windows it ships as a drop-in `winmm.dll`. Any program that calls the normal
Windows MIDI API (`midiOutOpen`, `midiOutShortMsg`, ...) uses SVMS instead of the
built-in synth, with no code changes. Programs that want more control can use
the native SVMS API, a KDMAPI-compatible facade, or a pluggable backend router.

V3 is under active development. Interfaces, configuration fields and audio
behavior may change before the first stable release, and there are no official
binary releases yet. Build it from source.

## Features

- Exact output-frame event scheduling with stable ordering for equal-frame events
- SF2 playback on Scalar, SSE2 and AVX2 render backends, with a multicore worker pool
- Voice stealing, optional per-key voice caps and voice decimation for extreme loads
- Priority and lossless MIDI-ingress modes
- Controllers, pitch bend, sustain, panic and common GM/GS/XG SysEx
- Reverb, lookahead limiter, optional per-channel limiters, post-output filtering
- Live SoundFont switching and priority-ordered SoundFont stacks
- Live post-DSP recording to 32-bit float WAV/RF64
- Offline MIDI-to-WAV renderer (GUI page and command line)
- Configurator with live controls, diagnostics and profile import/export
- WASAPI output (ASIO optional), Windows x64/x86/XP and Linux x86-64

The voice ceiling is a limit, not a promise: SoundFont layering, event rate, CPU
speed and buffer size decide how many voices play in real time.

## Build

### Windows

Needs Visual Studio 2022 or newer (Desktop development with C++), CMake and Ninja.

```bat
build.bat
```

| Command | Result |
| --- | --- |
| `build.bat` | x64 Release in `build\V3\bin` |
| `build.bat x86` | 32-bit build for 32-bit MIDI applications |
| `build.bat xp` | Windows XP-compatible x86 build, with tests |
| `build.bat xp-mingw` | XP build with MinGW (`W64DEVKIT_X86`, default `.\w64devkit-x86`) |
| `build.bat player` | Reference MIDI player |
| `build.bat clean` | Delete `build\V3` |

Set `CMAKE_BUILD_PARALLEL_LEVEL` to cap build parallelism on small machines.

The x64 build produces:

| File | Purpose |
| --- | --- |
| `winmm.dll` | Drop-in WinMM MIDI driver |
| `SVMSAPI.dll` | Standalone native-API and KDMAPI-compatible runtime |
| `OmniMIDI.dll`, `SnappySynth.dll`, `SVMS.dll` | The WinMM runtime under other names, for hosts that load those |
| `SVMSConfigurator.exe` | Configuration, diagnostics and offline-render UI |
| `svms_v3_render.exe` | Command-line MIDI-to-WAV/RF64 renderer |

### Linux

Needs a C++ compiler, CMake, Ninja and the ALSA development package.

```sh
sudo apt-get install build-essential cmake ninja-build libasound2-dev
./build.sh
```

This builds and tests `svmsd` (an ALSA sequencer-to-PCM daemon), `svms_v3_render`,
`libsvmsapi.so` and `libOmniMIDI.so`. Connect a MIDI source to the `svmsd` port
with `aconnect`.

## Use the Windows driver

1. Pick the DLL that matches your MIDI program (32-bit or 64-bit).
2. Copy `winmm.dll` next to the program's executable.
3. Put an `.sf2` next to the DLL, or choose one in the Configurator.
4. Start the program.

The driver reads `config.json` next to its DLL, then
`%APPDATA%\SuperVirtualMIDISynth\config.json`, and creates a default one if
needed. Relative SoundFont paths resolve from the configuration directory. The
Configurator edits the same file; press **Save Configuration** to apply changes.

## Integrate with your own synth or player

Pick the surface that fits.

- **Nothing to change.** Call the Windows MIDI API as usual and drop in `winmm.dll`.
- **Native API.** Include `src/include/svmsapi.h` and load `SVMSAPI.dll` (or
  `libsvmsapi.so`). It uses sized C structures, capability flags, timestamped
  batches, isolated offline sessions and caller-owned output buffers.
- **KDMAPI.** `SVMSAPI.dll` and `OmniMIDI.dll` export the KDMAPI function set.
- **Single header.** `src/frontends/SVMSAPI.h` binds the KDMAPI-style exports through
  `LoadLibrary` and defines the contract for third-party synth backends.
- **Router.** With `api.backend` set, SVMSAPI forwards events to another synth
  DLL (SVMS-API or KDMAPI) or a WinMM device instead of rendering them itself.

Read [src/include/README.md](src/include/README.md) before integrating. It
documents every function, the capability negotiation and the discovery rules.

## Offline rendering

```bat
build\V3\bin\svms_v3_render.exe input.mid soundfont.sf2 output.wav
```

Run it with no arguments to list the sample-rate, voice-count, thread, backend and
limiter options. The Configurator's **Offline Renderer** page uses the same engine.

## Tests

```bat
ctest --test-dir build\V3 --output-on-failure
```

The suite covers event timing and ordering, SF2 behavior, the voice lifecycle,
limiting, native-API ABI compatibility, configuration profiles and multi-process use.

## Layout

| Path | Contents |
| --- | --- |
| `src/synth` | Core types, configuration, SoundFont parser, voice manager, offline renderer |
| `src/render` | Render kernels (Scalar/SSE2/AVX2), worker pool, limiter, reverb, filters, GPU experiments |
| `src/events` | MIDI ingress, event compiler, scheduler, queues, SysEx |
| `src/driver` | Driver core, audio output (WASAPI/ASIO/DirectSound), runtime link, DLL entry |
| `src/frontends` | WinMM, KDMAPI, native-API and BASS export surfaces, Linux daemon |
| `src/configurator` | Configurator UI |
| `src/canonical` | Canonical reference engine |
| `src/player` | Reference MIDI player |
| `src/include` | Public API header and integration guide |
| `src/tests` | Correctness suite, benches and smoke tests |
| `third_party` | Vendored Dear ImGui (used backends) and nlohmann/json |
| `asio-main` | Steinberg ASIO SDK sources (GPLv3 option) for the optional ASIO output |
| `.github/workflows` | Windows and Linux CI |

Earlier generations live on separate branches: `v1` (TinySoundFont) and `v2`
(BASSMIDI). `v3` is the current code.

## License

GNU General Public License v3, see [LICENSE](LICENSE). Vendored dependencies keep
their own licenses.
