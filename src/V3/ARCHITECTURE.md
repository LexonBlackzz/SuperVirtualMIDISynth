# SuperVirtualMIDISynth V3 Architecture

## Goals

V3 is a Windows `winmm.dll` replacement for extreme-density MIDI playback.
The long-term modern-CPU stress target is approximately 500,000 simultaneously
active voices while accepting bursts of tens of millions of MIDI note events
per second, with an Intel i5-13600KF-class system representing the intended
high-end benchmark tier. The scalar renderer is also a compatibility baseline
that must remain useful on very old CPUs such as the Intel Celeron 420; SIMD
and parallel backends are later accelerators, not prerequisites for
correctness.

The design therefore separates three problems:

1. MIDI ingestion and scheduling must absorb dense event bursts without taking
   locks or allocating on the audio thread.
2. Voice state must be compact and cheap to advance, even when most voices are
   audibly culled under extreme pressure.
3. Audio mixing must spend memory bandwidth and interpolation work primarily on
   voices that contribute the most perceptual energy.

## Runtime Pipeline

```
MIDI application
    |
    v
winmm exports (midiOutShortMsg, KDMAPI, etc.)
    |
    v
Timestamped SPSC queue, 16,384 entries
    |
    v
WASAPI audio callback
    |
    +-- drain queue into persistent pending-event storage
    +-- convert QPC timestamps using the virtual render clock
    +-- sort events by fractional sample offset
    +-- dispatch events at their exact frame boundary
    |
    v
RenderScalar::RenderBlock
    |
    +-- refresh channel-dependent mix gains once per block
    +-- sort active voices by velocity when decimation is active
    +-- advance every voice's phase, envelope, and retirement state
    +-- fetch/mix audio only for selected voices
    +-- swap-remove retired voices from the active list
    |
    v
planar stereo mix -> interleaved output -> limiter -> WASAPI
```

The audio thread performs MIDI dispatch and rendering directly. The current
implementation is single-threaded to keep scheduling deterministic and avoid
cross-worker synchronization overhead on low-end CPUs.

## Event Ingestion and Scheduling

The producer-facing MIDI path uses a bounded lock-free SPSC queue. Events are
timestamped with `QueryPerformanceCounter`; the audio thread converts them to
fractional sample offsets against a monotonically advancing virtual render
clock. This prevents callback timing and buffer boundaries from quantizing
events into audible timing clusters.

The audio thread maintains a persistent pending-event buffer. Future events
remain queued across callbacks, while in-block events are sorted and dispatched
inside the render loop. The current event capacity is 2,097,152 entries. The
queue is bounded by design: if a producer outruns the consumer indefinitely,
backpressure and eventual event dropping are preferable to unbounded memory
growth or audio-thread allocation.

The tens-of-millions-of-events-per-second goal will require further work beyond
the current insertion-sort and per-event dispatch path. Likely future work
includes bulk MIDI packing, event coalescing for redundant controller changes,
radix/bucket scheduling, and a separate overload policy for events versus
voices.

## Voice Pool

Voice state is stored in `VoiceSoA`. Its hot field arrays share one 64-byte-
aligned allocation sized to the configured voice capacity, so the default
1000-voice pool no longer pays for 4096 entries. The current hard ceiling is
still 4096 because several lifecycle and stealing indices in `VoiceManager`
remain fixed-capacity. Those indices must also become capacity-sized or paged
before the pool can grow toward the 500K target.

The current pool uses:

- `activeList_[0..activeCount_)` for all active and releasing voices
- a LIFO `freeStack_` for O(1) allocation
- per-channel/key linked heads for note-off and panic operations
- swap-remove retirement
- score-based stealing when the configured pool is full

Every note-on gets a fresh voice. Same-note retriggers overlap naturally;
voice pressure is handled by stealing rather than recycling an existing key.

The steal score favors removing releasing, quiet, and older voices while
protecting attacks and louder voices.

## Scalar Renderer

`RenderScalar::RenderBlock` uses a per-sample outer loop. At each sample it:

1. dispatches all events whose fractional offset falls at that frame;
2. determines the current decimation step;
3. advances every active voice;
4. fetches and mixes samples only for the selected voices; and
5. retires voices that reach the end of their sample or release tail.

The fused per-voice body avoids a function call and argument marshaling in the
hot loop. Sustained envelopes use an early fast path, releasing envelopes use
one decay multiply, and sample-loop bounds are precomputed at note-on. Channel
pan and volume are premultiplied into per-voice gains once per block.

Voices that are not mixed still advance their phase and envelope. This avoids
creating frozen voices that occupy the pool forever.

## Adaptive Decimation

The current thresholds are:

| Active voices | Step | Selected mix voices |
|---:|---:|---:|
| `< 2,000` | 1 | 100% |
| `< 50,000` | 2 | approximately 50% |
| `< 150,000` | 4 | approximately 25% |
| `< 500,000` | 8 | approximately 12.5% |
| `>= 500,000` | 16 | approximately 6.25% |

The active list is velocity-sorted when step is greater than one, so loud
voices are retained ahead of quiet voices. Newborn voices are temporarily
protected. These are the first density controls; reaching 500K voices will
require a more scalable active-list representation and likely energy-based
selection rather than a full sort.

## CPU Tiers and Celeron 420 Constraints

The project has two distinct performance targets:

- Modern multicore CPUs such as the i5-13600KF: the 500K-voice and
  tens-of-millions-of-events stress target, using scalable storage, scheduling,
  parallelism, and SIMD where available.
- Legacy scalar CPUs such as the Celeron 420: compatibility and graceful
  operation at practical voice counts, with the scalar renderer providing the
  reference path.

The Celeron 420 is a single-core, in-order-era baseline with limited cache and
memory bandwidth. Scalar performance priorities on that tier are therefore:

- no per-sample heap work, locks, virtual calls, or division in the full-mix
  path;
- dense SoA arrays and predictable linear traversal;
- precomputed sample-loop and gain values;
- no work for voices that are below the selected mixing budget, except state
  advancement required for correct retirement;
- bounded event processing and explicit overload behavior; and
- deterministic operation suitable for profiling without SIMD assumptions.

The Celeron tier is not expected to render 500K voices in real time. The
eventual modern-CPU design will still benefit from tiered state updates:
audible voices at full rate, quiet voices at reduced control/audio rates, and
dormant tails represented compactly until they become audible or retire.

## File Map

```
SVMSTypes.h          shared POD types, constants, voice arrays, decimation
SVMSVoiceManager.h   allocation, stealing, active/free pools, retirement
SVMSRenderScalar.h   fused scalar renderer and per-sample event dispatch
SVMSDriver.h/cpp     winmm exports, event timing, MIDI dispatch, limiter
SVMSPSCQueue.h       lock-free producer/consumer MIDI queue
SVMSChannelCache.h   per-channel parameter snapshots
SVMSSoundFont.*      SF2 parsing, regions, and sample data
SVMSAudioOutput.h    WASAPI shared-mode output
SVMSDiagWindow.*     runtime diagnostic window
SVMSConfig.*         engine configuration and snapshots
```

See `ROADMAP.md` for the path from the current 4K scalar baseline to the
500K/event-flood target.
