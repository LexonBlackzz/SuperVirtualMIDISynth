# SuperVirtualMIDISynth

SuperVirtualMIDISynth V3 is an experimental SoundFont MIDI synthesizer built for
sample-accurate playback, dense MIDI streams, and high polyphony. On Windows it
can run as a drop-in `winmm.dll`, so applications can use it without adding a
custom audio backend.

V3 remains under active development. Interfaces, configuration fields, and
audio behavior may change before the first stable release. The project does not
publish official V3 DLL releases yet, but you can build and test it from source.

## Current V3 features

- Exact output-frame event scheduling with stable equal-frame ordering
- SF2 playback with scalar, SSE2, and AVX2 render backends
- Optional multicore voice rendering with deterministic tile reduction
- Configurable voice pools from small legacy-PC setups to a 524,288-voice ceiling
- Priority and lossless event-ingress modes
- Channel controllers, pitch bend and range changes, sustain, panic, and common
  GM/GS/XG SysEx state
- Configurable reverb, lookahead limiter, and post-output high-pass filtering
- JSON configuration with local and Roaming AppData lookup
- Configurator with live controls, diagnostics, profile import/export, and an
  offline renderer page
- Live post-DSP recording to stereo 32-bit float WAV/RF64
- Live SoundFont switching with off-thread preparation and block-boundary activation
- Native SVMS API and KDMAPI-compatible entry points
- Windows x64, Windows x86, Windows XP x86, and Linux x86-64 targets

The configured voice ceiling does not guarantee real-time playback at that
voice count. SoundFont layering, event rate, CPU speed, audio buffer size, and
voice stealing can change the cost by a large amount.

## Build on Windows

Install:

- Visual Studio 2022 or newer with Desktop development with C++
- CMake 3.20 or newer
- Ninja

Run the build script from the repository root:

```bat
build_v3.bat
```

The x64 build goes to `build\V3\bin`. Its main files are:

| File | Purpose |
| --- | --- |
| `winmm.dll` | Drop-in WinMM MIDI output driver |
| `SVMSAPI.dll` | Native SVMS API runtime |
| `OmniMIDI.dll` | KDMAPI-compatible runtime name |
| `SnappySynth.dll` | Ziggy-compatible runtime name |
| `svms_v3_configurator.exe` | Configuration, diagnostics, and offline-render UI |
| `svms_v3_render.exe` | Standalone MIDI-to-WAV/RF64 renderer |

The DLL names above contain the same runtime. Each name exists so applications
that expect a particular integration can load V3 without creating separate
synth engines inside one process.

### Other Windows targets

```bat
build_v3_x86.bat
build_v3_xp_x86.bat
```

Use the x86 build with 32-bit MIDI applications. The XP script expects an x86
MinGW toolchain at `w64devkit-x86`, excludes AVX2, and uses the legacy
audio-output path. The modern Configurator and offline-render executable do not
ship in the XP target.

## Use the Windows driver

1. Match the DLL architecture to the MIDI application.
2. Copy `winmm.dll` beside the application's executable.
3. Put an SF2 file beside the DLL or select one in the Configurator.
4. Start the MIDI application.

V3 checks for `config.json` beside its DLL first. If that file does not exist,
it uses `%APPDATA%\SuperVirtualMIDISynth\config.json` and creates a default
configuration when possible. Relative SoundFont paths resolve from the selected
configuration directory. With no configured SoundFont, V3 searches beside the
DLL for a local `.sf2` file.

The Configurator edits the same JSON file and can import or export complete JSON
profiles from **Advanced > Configuration**. Import changes the working copy;
press **Save Configuration** to write and apply it.

The **MIDI** page can route a physical MIDI input directly into SVMS. Enable
it, select the system input, save, and restart the host. Short messages retain
their arrival timestamps and SysEx uses fixed reusable input buffers; normal
host `midiIn*` forwarding remains independent.

The **Live Recording** page records the final stream after reverb, limiting,
and post filtering. V3 writes the file on a background thread and reports any
frames dropped because the disk could not keep up.

The **Audio** page can load the selected SoundFont into the running driver.
Parsing and sample preparation happen on a loader thread; the finished immutable
bank activates at the next audio-block boundary without restarting WASAPI. Any
voices using the previous bank are silenced at activation while MIDI channel
state is retained. Save the configuration separately to reuse the bank later.

## Offline rendering

Open the Configurator's **Offline Renderer** page to select a MIDI file,
SoundFont, and output WAV. The page shows load/render progress, speed, ETA,
voice counts, and steals. You can cancel without freezing the Configurator.

The command-line renderer exposes the same engine and WAV/RF64 writer:

```bat
build\V3\bin\svms_v3_render.exe input.mid soundfont.sf2 output.wav
```

Run it with no arguments to see options for sample rate, voice count, render
threads, backend selection, limiter settings, event-buffer size, and scan-only
MIDI validation.

## Build on Linux

Install a C++ compiler, CMake, Ninja, and the ALSA development package. On
Ubuntu or Debian:

```sh
sudo apt-get install build-essential cmake ninja-build libasound2-dev
./build_v3_linux.sh
```

The Linux build provides:

- `svmsd`, an ALSA Sequencer to ALSA PCM real-time daemon
- `svms_v3_render`, the offline renderer
- `libsvmsapi.so`, the native API runtime
- `libOmniMIDI.so`, a compatibility name used by Linux Ziggy

See [src/V3/LINUX.md](src/V3/LINUX.md) for setup, MIDI-port connection, and
Ziggy instructions. GitHub Actions also builds and tests an Ubuntu 18.04
compatible x86-64 archive.

## Native API

New applications can include `src/V3/include/svmsapi.h` and load
`SVMSAPI.dll` or `libsvmsapi.so`. ABI 1 uses sized C structures, capability
flags, timestamped batches, isolated offline sessions, and caller-owned output
buffers.

Read [src/V3/include/README.md](src/V3/include/README.md) before integrating.
Applications should request only the capabilities they use and accept a shorter
function table from older runtimes.

## Tests

The normal Windows and Linux builds enable tests. Run:

```bat
ctest --test-dir build\V3 --output-on-failure
```

The suite covers event timing and ordering, SF2 behavior, voice lifecycle,
limiting, RuntimeLink/native ABI compatibility, configuration profiles, and
multi-process runtime use.

## Project layout

| Path | Contents |
| --- | --- |
| `src/V3` | V3 driver, synth engine, Configurator, renderer, and tests |
| `src/V3/include` | Public SVMS API header and integration guide |
| `src` | Earlier SVMS implementations and supporting projects |
| `third_party` | Vendored dependencies and their licenses |
| `.github/workflows` | Continuous-integration builds |

## License

SuperVirtualMIDISynth uses the [GNU General Public License v3](LICENSE).
Vendored dependencies retain their own licenses.
