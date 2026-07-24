# SuperVirtualMIDISynth V3 Roadmap

## Phase 1: Foundation & 64-Voice Playback (current)

- [x] Project scaffold (CMake, build_v3.bat)
- [x] SVMSTypes.h — all shared types and constants
- [x] SVMSConfig.h/cpp — engine configuration
- [x] SVMSSoundFont.h/cpp — SF2 RIFF parser + resampler
- [x] SVMSPageAllocator.h — sample page allocator
- [x] SVMSVoiceManager.h — SoA voice pool (64 voices)
- [x] SVMSChannelCache.h — per-channel parameter cache
- [x] SVMSEnvelope.h — ADSR envelope
- [x] SVMSRenderScalar.h — scalar render path
- [x] SVMSAudioOutput.h — WASAPI shared mode
- [x] SVMSDriver.h/cpp — winmm.dll exports + engine glue
- [ ] Build and verify 64-voice playback

## Phase 2: Core Engine

- [ ] Timing wheel scheduler for sample-accurate dispatch
- [x] Per-key transition queues (same-key retriggers)
- [x] Sub-block event scheduling
- [ ] Multi-worker thread pool
- [ ] Per-worker mix buffers with AVX accumulation
- [ ] Tile-based render orchestration
- [ ] Adaptive decimation (SnappySynth-style)
- [ ] Overload ladder (soft/hard/panic pressure)
- [x] Offline deterministic render path

## Phase 3: SoundFont Runtime

- [ ] Complete SF2 zone matching (key/velocity ranges)
- [ ] Default modulator set (velocity→attenuation, pitch→filter, etc.)
- [ ] LFO (vibrato, tremolo, filter modulation)
- [ ] Reverb (FDN-based)
- [ ] Chorus (modulated delay)
- [ ] 2-pole/4-pole resonant LPF
- [ ] Sustain pedal with note-off tracking
- [ ] Sostenuto pedal
- [ ] Velocity curve mapping
- [ ] Per-voice pan from soundfont

## Phase 4: SFZ + DLS

- [ ] SFZ text parser (key opcodes)
- [ ] SFZ region matching
- [ ] SFZ opcode: sample, key, velocity, EG, filter, LFO
- [ ] SFZ group/off_by for hi-hat choke
- [ ] DLS RIFF parser (reuse SF2 infrastructure)
- [ ] DLS articulator support (level 1)

## Phase 5: SIMD Acceleration

- [ ] SSE2 render path (4 voices at a time)
- [ ] AVX2 render path (8 voices at a time)
- [ ] AVX-512 render path (16 voices at a time)
- [ ] Auto-detection of CPU features at startup
- [ ] Fallback chain: AVX-512 → AVX2 → SSE2 → Scalar
- [ ] Linear interpolation for decimated voices
- [ ] Prefetch orchestration for sample pages

## Phase 6: GPU Backend

- [ ] CUDA backend: sample upload, persistent kernel
- [ ] CUDA density tier: spectral mass rendering
- [ ] Vulkan compute backend
- [ ] GPU device selection and fallback
- [ ] CPU-GPU sync pipeline
- [ ] Config toggle

## Phase 7: Configurator + Telemetry

- [ ] Shared memory telemetry (voice count, CPU, buffer stats)
- [ ] Dear ImGui-based configurator
- [ ] Basic / Advanced / Expert settings tabs
- [ ] Live diagnostics graphs
- [ ] SoundFont browser
- [ ] Profile import/export

## Phase 8: Polish + Extras

- [ ] WASAPI exclusive mode
- [ ] ASIO support (via ASIO SDK)
- [ ] midiStream* API functions
- [ ] Offline render mode (MIDI → WAV)
- [ ] Record-to-WAV (record real-time output)
- [ ] Deterministic test harness
- [ ] Black MIDI stress corpus
- [ ] Performance benchmark suite

## Phase 9: Cross-Platform (future)

- [ ] Linux: ALSA + PulseAudio backends
- [ ] WINE compatibility testing
- [ ] Android: Oboe audio, NDK build
- [ ] macOS: CoreAudio backend
