# SVMS V4 Rewrite

A small offline C++20 sample synthesizer built with ONLY one architecture: exact
sample-position event spans feeding a flat voice SoA, with scalar, AVX2, and
persistent-worker execution of the same semantics.

The core stays format-agnostic. A separate dependency-free preparation layer
loads a practical SF2 subset, parses Standard MIDI Files, and expands immutable
regions into the same exact-frame events used by generated tests. Parsed MIDI is
retained as a compact frame-grouped byte stream and decoded sequentially during
preparation rather than stored as one C++ struct per message.

## Build and run

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
.\build\midisynth_bench.exe --runs 5 --warmups 1 --suite core
.\build\midisynth_midi_bench.exe
```

Benchmark suites are `core`, `workers`, `tiles`, `diagnostic`, and `all`.
`--detailed` separates tile execution/dispatch from deterministic reduction;
`--help` lists optimization A/B switches and the worker scheduling threshold.

Add an external real-music workload without replacing generated baselines:

```powershell
.\build\midisynth_bench.exe --suite real --runs 3 --warmups 1 `
  --sf2 C:\sounds\bank.sf2 --midi C:\midi\song.mid
```

## Offline SF2 + MIDI rendering

```powershell
.\build\midisynth_render.exe `
  --sf2 C:\sounds\bank.sf2 `
  --midi C:\midi\song.mid `
  --output song.wav `
  --sample-rate 44100 `
  --backend scalar `
  --workers 3 `
  --tile-size 512 `
  --voice-capacity 65536 `
  --format float32
```

Float32 IEEE WAV is the default development output. It preserves the synth's
floating-point samples, including values outside `[-1, 1]`, and interleaves the
stereo channels only while writing. Use `--format pcm16` for explicitly clamped
compatibility output. The renderer reports the selected format, wall duration,
real-time factor, peak voices, source and launched NoteOns, dropped NoteOns, EVT,
and SYN. SMF format
0/1, tempo changes, NoteOn/Off, bank select, program change, sustain, channel
volume/pan/expression, pitch bend, RPN pitch-bend sensitivity, reset, all-notes-off,
and all-sound-off are supported. Other controllers and unsupported SF2
generators are summarized by name.

SF2 volume Delay/Attack/Hold/Decay/Sustain/Release is supported in the canonical
renderer. Velocity intentionally uses `(velocity/127)^2`, without stacking the
SF2 default velocity-attenuation modulator, and region pan uses a constant-power
law. Filters, explicit modulators, LFOs, modulation envelopes, effects sends,
exclusive class, and release-loop exit remain reported limitations. See
[ARCHITECTURE.md](ARCHITECTURE.md) for the boundary and [VST3_PLAN.md](VST3_PLAN.md)
for the deferred DAW adapter.

Set `MIDISYNTH_ENABLE_AVX2=OFF` at configure time for a scalar-only binary. At
runtime, `SynthConfig::backend` independently selects scalar or AVX2 and
`workerThreads` independently selects the number of persistent background
workers. Requesting AVX2 on an unsupported CPU/build reports an error; voices are
never silently diverted to another renderer.

## Minimal use

```cpp
midisynth::SampleBank bank;
std::vector<float> wave = /* mono sample data */;
auto sample = bank.addLoop(wave, 0, static_cast<std::uint32_t>(wave.size()));

midisynth::Synth synth(bank, {
    .voiceCapacity = 1024,
    .tileSize = 512,
    .maxBlockFrames = 4096,
    .workerThreads = 0,
    .backend = midisynth::Backend::Scalar,
});

std::vector events{
    midisynth::Event::noteOn(128, 0, 60, sample),
    midisynth::Event::noteOff(512, 1, 60),
}; // sorted by (frame, sequence)
std::vector<float> left(1024), right(1024);
synth.render(0, events, left, right);
```