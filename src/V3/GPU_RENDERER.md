# V3 GPU Synthesis — Contract & Milestones

Status: **design, first real step in progress**. Branch: `v3-gpu`.

## The decision in one sentence

V3's existing host keeps **all** scheduling, voice-lifecycle, and event-timing
semantics exactly as they are; the GPU is added as a **pure sample-synthesis
stage** fed a fully-resolved, per-block, per-voice plan. The GPU is a renderer,
never a scheduler.

## Why the GPU must never become a scheduler

Kestrel's peak throughput comes from two choices that conflict with the sound
V3 guarantees:

1. **Admission dropping** — most note-ons are discarded before ever becoming a
   voice (their worst case drops 83.7M of ~89M note-ons). V3 gives every note-on
   a real voice and fights for survival via stealing. That guarantee stays.
2. **Tiled gates** — note-off is resolved on a ~32-frame `GATE_TILE`, capping
   retrigger resolution near ~1500 Hz. V3 resolves note boundaries on their
   exact frame with sub-sample start, which is why both a 60 Hz and a 4000 Hz
   hum reproduce exactly. That exactness stays host-side.

Kestrel's GPU renderer (the DSP, not the scheduler) is nevertheless the right
reference for the per-voice per-sample loop, which is SIMT-friendly and
sample-exact by construction.

## What the GPU consumes

A per-block directive already produced by the unchanged host:

- per-voice synthesis params read from `VoiceSoA` (phase, step, current gain,
  mix gain L/R, sample region, loop bounds, envelope level, release frame);
- the float sample pool for the loaded SoundFont;
- the stereo frame budget for the block.

The host retains exact-frame dispatch, CC-split-at-frame, sub-sample note-on
phase fold, per-voice exact release frame, and stealing. This is identical for
every frontend because they all reach the same scheduler/voice-manager.

## Frontends

WinMM, KDMAPI, the native **SVMS API**, and the offline renderer all feed the
same scheduler. A GPU backend placed behind `VoiceManager` therefore benefits
all of them at once, and keeps the SVMS API as cheap as it is today (the API
never touches per-voice DSP).

## Parity target (agreed)

- **Not** bit-identity. V3's scalar/SSE2/AVX2 backends are already
  not bit-identical to each other.
- **Gate: ~ -85 dB peak difference** between the GPU render and the equivalent
  CPU scalar render of the same block/state. Below human audibility; preserves
  character.

## Compute API choice

D3D11 compute (`cs_5_0`, `d3dcompiler`). Already linked by the configurator
(`d3d11 dxgi`), no new heavyweight dependency, works on the RTX 3060 dev target,
and supports a testing/realtime path on any modern Windows (no XP support by
definition — the XP/DirectSound path stays fully on CPU).

The HLSL DSP core (dereference, linear/Catmull-Rom interpolation, envelope
multiply, gain, and a deterministic accumulation) is a near 1:1 transcription of
Kestrel's `render.wgsl` minus the parity scaffolding (`quantise_level`, Kahan,
`unpack2x16snorm`) that -85 dB makes unnecessary.

## Determinism & accumulation

- Fixed-point (or f32) phase accumulated per voice, in a fixed per-voice order.
- Cross-voice sum is **deterministic**: a fixed reduction order (per-workgroup
  partials then a per-frame reduce), **never** non-deterministic float atomics.
- Performance notes:
  - sample pool stays f32 (network of 3060 ~360 GB/s easily covers 300K voices
    at 2-tap linear; the 16-bit packed pool Kestrel uses would add per-sample
    noise that fights the -85 dB gate).
  - full per-frame per-voice partials do not scale to 100K voices; the reduce
    dimension is sized per workgroup (partials[f][workgroup]) exactly as
    Kestrel does.

## Voice scaling targets (agreed)

Realtime, every-note-on + sample-exact held:
- 25K: first proof; comfortably in reach, any modern discrete GPU.
- 50K min / 100K nice / 300K goal.
- 500K: "later"; likely bound by the **host** steal scan, not the GPU.

The main host-side risk after offload is the O(activeCount) steal scan under
retrigger storms; that is a separate host-side rework, tracked in ROADMAP.

## Milestones

1. **Proof spike** (`svms_v3_gpu_spike`): D3D11 compute renders a synthetic
   voice set to WAV; CPU-scalar reference run prints peak dB diff against the
   -85 dB gate. De-risks the shader DSP + device plumbing in isolation.
2. **Offline integration**: `svms_v3_render --backend gpu` routes bulk
   synthesis through the GPU behind `VoiceManager`; parity harness diffs
   CPU-render vs GPU-render on a real file/soundfont.
3. **Realtime**: one-block-ahead pipelined GPU renderer feeding the existing
   limiter → WASAPI, host scheduling unchanged, no blocking readback inside the
   audio callback.