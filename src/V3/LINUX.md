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
`svmsd`, the offline `svms_v3_render` tool, and the native Ziggy-compatible
`libOmniMIDI.so` KDMAPI library.

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

The daemon target accepts MIDI 1.0 channel voice messages through ALSA
Sequencer. Native JACK/PipeWire MIDI, SysEx, service installation, and a GUI
are later work.

## Ziggy and KDMAPI

Place `libOmniMIDI.so` beside the Linux `ziggy` executable. Ziggy discovers it
automatically and uses the same KDMAPI calls as its Windows real-time mode:

```sh
cp /path/to/libOmniMIDI.so /path/to/ziggy-directory/
cd /path/to/ziggy-directory
./ziggy
```

The library searches for `config.json` in the working directory, beside the
library, and in `$XDG_CONFIG_HOME/SuperVirtualMIDISynth` (or
`~/.config/SuperVirtualMIDISynth`). Relative SoundFont paths are resolved from
the selected configuration. With no configuration it looks for `gm.sf2`
beside Ziggy or the library. `SVMS_CONFIG`, `SVMS_SOUNDFONT`, and
`SVMS_AUDIO_DEVICE` provide explicit overrides.

Linux-specific ALSA output can optionally be selected without changing the
Windows device setting:

```json
{
  "audio": {
    "linux_device": "default"
  }
}
```

The compatibility exports include rendering time, active voices, free voices,
and voice steals. Ziggy therefore recognizes this library as an SSV2-style
provider and can show all four overlays.
