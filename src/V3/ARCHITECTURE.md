# SuperVirtualMIDISynth V3 Architecture

## Overview

V3 is a complete rewrite targeting extreme polyphony (500K voices) for Black MIDI
playback, built as a standalone `winmm.dll` drop-in MIDI driver.

## Core Pipeline

```
MIDI App → winmm.dll (midiOutShortMsg)
              │
              ▼
       SPSC Ring Buffer (lock-free, 16K entries)
              │
              ▼
    ProcessMidiEvents (audio thread)
       ├─ NoteOn → VoiceManager::AllocateVoice
       ├─ NoteOff → VoiceManager::StartRelease
       ├─ CC → ChannelCache
       └─ PitchBend → ChannelCache
              │
              ▼
    ChannelCache::RebuildCache (once per block)
       ├─ Volume, expression, pan per channel
       ├─ Pitch bend → cents
       ├─ Sustain pedal state
       └─ Filter params (future)
              │
              ▼
    RenderScalar::RenderBlock (per block, 64 voices)
       ├─ For each active voice:
       │   ├─ Read sample from SF2 data
       │   ├─ Apply ADSR envelope
       │   ├─ Apply channel gain, pan, velocity
       │   └─ Mix into stereo output
       └─ Apply master volume
              │
              ▼
    WASAPI Shared Mode → Audio Device
```

## Voice Management (SoA)

All voice state is stored as Structure-of-Arrays for cache efficiency and
SIMD readiness:

```
VoiceSoA {
    phases[N]          — phase accumulator per voice
    phaseIncs[N]       — pitch ratio per voice
    envLevels[N]       — current envelope level
    envSlopes[N]       — envelope step per sample
    envTargets[N]      — target envelope level
    gainLeft[N]        — left channel gain
    gainRight[N]       — right channel gain
    sampleStart[N]     — sample start offset
    samplePos[N]       — current playback position
    sampleEnd[N]       — sample end offset
    loopStart[N]       — loop region start
    loopEnd[N]         — loop region end
    loopMode[N]        — 0=no loop, 1=loop
    channel[N]         — MIDI channel
    note[N]            — MIDI note number
    velocity[N]        — MIDI velocity
    state[N]           — Free/Active/Releasing
    envPhase[N]        — envelope phase state
    samplePageId[N]    — sample page reference (future)
}
```

## Adaptive Decimation (Phase 2)

SnappySynth-inspired quality reduction based on voice load:

| Active Voices | Decimation Step | Effective Sample Rate |
|---|---|---|
| < 20K | 1 (no decimation) | 44.1kHz |
| 20K–40K | 8 | 5.5kHz |
| 40K–80K | 16 | 2.7kHz |
| 80K–160K | 32 | 1.4kHz |
| 160K+ | 64 | 689Hz |

Decimated samples use zero-order hold (or linear interpolation in Phase 5).

## Phase Plan

See ROADMAP.md for the full phased implementation plan.

## File Map

```
src/V3/
├── SVMSTypes.h            — All POD types, enums, constants
├── SVMSConfig.h/cpp       — Engine configuration and runtime snapshot
├── SVMSSoundFont.h/cpp    — SF2 RIFF parser, resampler, zone matching
├── SVMSPageAllocator.h    — Sample page pool (4096-sample pages)
├── SVMSVoiceManager.h     — SoA voice pool (64 voices baseline)
├── SVMSChannelCache.h     — Per-channel param pre-compute
├── SVMSEnvelope.h         — ADSR envelope generators
├── SVMSRenderScalar.h     — Scalar render path (baseline)
├── SVMSAudioOutput.h      — WASAPI shared mode output
├── SVMSDriver.h/cpp       — winmm.dll exports + engine glue
├── CMakeLists.txt         — Build configuration
├── ARCHITECTURE.md        — This file
└── ROADMAP.md             — Full implementation roadmap
```
