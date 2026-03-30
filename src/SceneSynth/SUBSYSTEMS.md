# SceneSynth Subsystems

## 1. Runtime Object Model

Planned core runtime objects:

- `EngineState`
- `SchedulerState`
- `SceneState`
- `ChannelState[16]`
- `KeyState[16][128]`
- `ExactVoicePool`
- `GroupedObjectPool`
- `DensityObjectPool`
- `TileRenderState`
- `TelemetryState`

Proposed implementation split:

- `VirtuallySuperTypes.h`
- `VirtuallySuperEngine.h/.cpp`
- `VirtuallySuperScene.h/.cpp`
- `VirtuallySuperPools.h/.cpp`

## 2. Scheduler

Responsibilities:

- ingest normalized MIDI events
- keep same-key pending transition queues
- collapse event floods into grouped or density candidates
- preserve exact same-key alternations when musically important
- apply overload-aware admission before full allocation

Planned structures:

- fixed-capacity event rings
- min-heap or calendar structure for far-future events
- per-key local transition queues for near-window accuracy
- per-channel density counters and burst detectors

Proposed implementation split:

- `VirtuallySuperScheduler.h/.cpp`
- `VirtuallySuperSchedulerQueues.h/.cpp`
- `VirtuallySuperSchedulerReduce.h/.cpp`

## 3. Scene Compiler

Responsibilities:

- convert scheduled events into exact/grouped/density scene updates
- score importance
- decide promotions and demotions between tiers
- produce tile jobs for render workers

Planned decisions:

- exact note attack vs grouped swarm attack
- tail fusion eligibility
- density substitution eligibility
- per-channel layer reduction

Proposed implementation split:

- `VirtuallySuperScene.h/.cpp`
- `VirtuallySuperSceneReduce.h/.cpp`
- `VirtuallySuperSceneScore.h/.cpp`

## 4. Exact Voice System

Responsibilities:

- hold literal per-note playback
- preserve buzz articulation
- carry per-note divergence and MPE-promoted notes

Desired behavior:

- no global scans for note-off
- no global scans for stealing
- bounded heaps or queues for steal candidates

Proposed implementation split:

- `VirtuallySuperExact.h/.cpp`
- `VirtuallySuperExactVoices.h/.cpp`
- `VirtuallySuperExactSteal.h/.cpp`

## 5. Grouped Rendering System

Responsibilities:

- cluster many related notes into cheaper render objects
- share sample locality, modulation, or timing where safe
- improve cache behavior and SIMD lane utilization

Likely grouping axes:

- sample page
- pitch band
- channel
- layer template
- timing bucket

Proposed implementation split:

- `VirtuallySuperGrouped.h/.cpp`
- `VirtuallySuperGroupedBuckets.h/.cpp`

## 6. Density System

Responsibilities:

- represent background note mass when exact playback is no longer worthwhile
- preserve perceived brightness, width, density, and rhythm energy

Candidate approaches:

- granular cloud renderer
- grouped release tail bus
- pitch-bucket texture synthesis
- later spectral density experiment

Initial chosen direction:

- granular cloud renderer as the first density implementation

Proposed implementation split:

- `VirtuallySuperDensity.h/.cpp`
- `VirtuallySuperDensityGranular.h/.cpp`

## 7. Tile Renderer

Responsibilities:

- render fixed tiles such as `128` or `256` samples
- generate jobs suitable for worker threads
- keep merge windows cache-friendly

Design rules:

- job-based threading over giant full-block worker buffers
- contiguous or page-local sample work whenever possible
- exact, grouped, and density jobs can coexist in one tile
- SIMD helper specialization should be explicit rather than hidden inside one
  monolithic render loop

Proposed implementation split:

- `VirtuallySuperRender.h/.cpp`
- `VirtuallySuperRenderJobs.h/.cpp`
- `VirtuallySuperRenderWorkers.h/.cpp`

## 8. Voice And Layer Accounting

Responsibilities:

- maintain exact counts
- maintain grouped representation counts
- maintain voice-equivalent numbers

Metrics to expose:

- exact voices
- layer instances
- grouped objects
- density objects
- voice equivalent

## 9. Overload Controller

Responsibilities:

- observe render budget, event pressure, sustain pressure, and queue age
- move pressure first into culling and abstraction
- avoid flattening fresh retriggers unless truly necessary

Tools available to it:

- release shortening
- quiet-tail culling
- layer count reduction
- grouped promotion
- density substitution
- stale event rejection

Proposed implementation split:

- `VirtuallySuperOverload.h/.cpp`

## 10. Diagnostics Publisher

Responsibilities:

- expose fixed shared-memory snapshots
- expose lock-free event ring for coarse debug notifications
- never block the audio thread

Data classes:

- hot stats
- warm stats
- cold/static info

Proposed implementation split:

- `VirtuallySuperTelemetry.h/.cpp`
- `VirtuallySuperTelemetryShared.h`

## 11. Configurator

Responsibilities:

- display configuration and diagnostics
- graph histories
- browse fonts and folders
- format and rank data for humans

Non-goal:

- it must not force the engine to do expensive UI-oriented work

Related implementation split outside the engine folder:

- existing `Configurator.cpp` can later be refactored to consume
  `VirtuallySuper` telemetry without forcing UI code into the engine modules
