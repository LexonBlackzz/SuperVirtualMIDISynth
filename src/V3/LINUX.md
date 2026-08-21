# SVMS V3 on Linux

This is the first native Linux target. It exposes an ALSA Sequencer MIDI port,
renders with the same V3 SF2/voice/SIMD core as Windows, and sends audio to an
ALSA PCM device. PipeWire and PulseAudio normally expose compatible `default`
ALSA devices, so no app-specific driver shim is required.

## Build

Ubuntu/Debian prerequisites:

```sh
sudo apt-get install build-essential cmake ninja-build libasound2-dev
cmake -S src/V3 -B build/V3-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/V3-linux
ctest --test-dir build/V3-linux --output-on-failure
```

GitHub Actions also produces an Ubuntu 18.04-compatible archive containing
`svmsd` and the offline `svms_v3_render` tool.

## Run

```sh
./build/V3-linux/bin/svmsd --soundfont /path/to/gm.sf2
```

The daemon prints its ALSA client and port, for example `128:0`. Connect an
application with its own MIDI-output selector, or from a terminal:

```sh
aconnect -l
aconnect SOURCE_CLIENT:SOURCE_PORT 128:0
```

Useful options:

```sh
svmsd --soundfont gm.sf2 --max-voices 4096 --render-threads 0
svmsd --soundfont gm.sf2 --audio-device default --buffer-frames 2048
svmsd --help
```

`--render-threads 0` selects up to 16 logical processors. MIDI event ordering,
voice allocation, and stealing stay on the coordinator; helpers only render
deterministic voice tiles. Incoming events are assigned absolute output frames
from a monotonic-clock epoch plus the measured ALSA buffer lead. Events are not
snapped to render-block boundaries.

This initial target accepts MIDI 1.0 channel voice messages through ALSA
Sequencer. Native JACK/PipeWire MIDI, Linux JSON configuration, SysEx, service
installation, and a GUI are later work.
