# SVMS V3 — running changelog

Low-key running log, updated with every landed change. The root
`CHANGELOG.md` carries the polished per-version entries I publish when I cut
a release; this file is where changes land first, raw, under "Unreleased",
until I move them into a release section.

Format: newest first, one bullet per landed change, matching the commit's
`type(v3): one-liner` style plus a line of context where it helps.

## Unreleased

- 2026-10-17 feat(v3): configurator widgets for the Output, Tools and MIDI pages. `BeginNest/EndNest` (the indented rule for nested settings), `KeyValueGrid`, `StatGrid` (four stat tiles per row) and `StripedProgress`. Nothing uses them yet.
- 2026-10-17 feat(v3): configurator Sound page rewrite. One SoundFont list replaces the folder browser and the priority stack (drop .sf2/.sfz files on the window, drag rows to reorder, Load now, Remove; the Primary chip marks the first), velocity knobs with a smooth 120-point response graph that never animates while dragging, voice character with a description per mode, tuning, and the bank/preset routes disclosure. Velocity, tuning and phase rotation moved off the MIDI and Audio pages with the same sends; PageAudio is now only the output view until the Output rewrite. The velocity curve knob is logarithmic over the old 0.1 to 10 range.
- 2026-10-17 feat(v3): configurator Performance page rewrite. Profile cards (Accurate / Balanced / Extreme with Custom detection and staggered toggle flips), voice limit with the big number and the >64k warning, shortcut tiles in two columns grouped Free / Costs a little sound / Costs accuracy or timing with nested options only while their parent is on, CPU and threads, and the Engine internals disclosure. The Correctness mode switch is gone from the UI (the JSON key stays). The shortcut switches that lived on the MIDI page moved here with the same RuntimeLink sends; only Fast producer stays on MIDI. Tiles can now hold a slider instead of a toggle (minimum note length).
- 2026-10-17 `feat(v3): configurator Home page rewrite` - Home now matches the prototype: status hero, voices / render load / output tiles, SoundFont and profile cards, quick switches. Profile apply/match lives in PagePerformance so Home and Performance share it. Also added: card rows (equal heights), files dropped on the window, a field-style Select, and a shared live-command helper.
- 2026-10-17 feat(v3): configurator 0.11 foundations and new shell. Modern theme is the default (forced once via `ui_version` in the theme JSON; the Refined/"Boring" style is gone, style 1 now means Modern), Geist + Geist Mono are embedded (CMake-generated `GeistFonts.h`, system fonts as fallback), `Anim` gives per-ID springs/eases/pulses with a Full/Calm/Off motion level, `ModernWidgets` holds the new control set with Rack branches, and `SettingsRegistry` is the one table behind search, ↻ marks, save-bar counts and sidebar badges (restart flags checked against the real `Set*` sends; fast producer is live). The new shell has the 60 px top bar (EQ logo, connection pill, Applied-live chip, Voices/CPU/Output meters, master slider), the 232 px sidebar with search, spring indicator and badges, page-entry stagger, the floating Discard/Save bar and a top-centre toast. Adopt Engine is replaced by auto-adopt on connect. The old pages are mounted in the new nav until they are rewritten.

- 2026-10-16 feat(v3): offline renderer now matches the live synth's sound: reverb (before the limiter, tail rings out), velocity curve/floor/gate, mono output, pan law and interpolation come from the shared config, and the render backend follows config. The Configurator Offline tab hands its unsaved working settings to the renderer via `--config` and shows an "Engine settings used" panel (live-only items greyed). ReverbState is heap-allocated (it overflowed the test stack).

- 2026-10-16 perf(v3): offline renderer is single-pass and batched. A new `ParallelMidiDecoder` parses tracks in parallel one tick-window at a time and streams the exact (tick, track, order) event stream, so there is no 100 s up-front scan and rendering starts in seconds; the render loop hands each block's events to one RenderBlock call (`RealtimeBlockRenderer`) instead of re-entering per event. The standalone synth now applies the live tuning from the shared config (steal policy, per-key cap, ghost budget, decimation, retire floor, fast chunk math, affinity). WAV length is unknown while streaming, so the header reserves a JUNK slot and turns into RF64 on close. DYHTM Community Merge went from 13+ min to 4.4 min. `SVMS_OFFLINE_PER_EVENT=1` forces the old loop, `SVMS_OFFLINE_PROFILE=1` prints plan/jobs/post and collapse counters. A 524288-voice pool was 5x slower (21 min), so the cap stays. Reverb and the velocity settings are not in the offline path yet.

- 2026-10-16 feat(v3): opt-in minimum note length (events.min_note_ms, live, configurator slider) and an engine-shards slider in the configurator. The compiler moves a note-off that lands closer than the floor to its key's last note-on out to note-on + floor, so sub-block notes sound for at least that long instead of launching and releasing inside one block. Lowers note-end accuracy; the lever says so. Measured on DYHTM's finale (flow log, 0.25 s rows): the first burst is note-ons that live and die inside a block, and the late burst after it is 100% note-offs (zero note-ons), so the floor only helps the first. Shards slider is restart-applied (6 shards / 20 threads was best on the 20-thread CPU).

- 2026-10-16 perf(v3): fast chunk math now covers sustained and attack/decay chunks. Sustained advances phase per 8 frames in one multiply and masks its 1-7 frame tail; transient takes a closed-form attack ramp or decay ladder when the chunk stays in one stage without clamping. DYHTM 4:43-4:48 (6 shards, 16 threads): 62% -> 50% mean load with the toggle. The oracle's repeated float adds drift 0.9-3.5 frames over 2048 frames at large phases (`.perf/phasedrift.py`), the closed form 0.1-0.7, so the fast-mode wave tolerance in the differential is 4e-3, not 1e-4; lever text says so.

- 2026-10-16 `perf(v3): batch ingress for mixed events and per-shard batches, plain owner-thread counters` - Player profile (Kimrashi, heavy 4:34+ section): the sender thread was 92% busy and ~70% of it sat in the per-event submit. Two causes: (1) a batch with any note-off/controller/low velocity fell back to one `SubmitShortMsgAtQpcCancellable` per event, and with `synth.shards` the batch call routed every event individually; (2) the hot lines were locked RMWs (per-key window/stack counters in the note-on collapse gate, coalesced/not-accepted stat counters). Now: `PrepareShortEvent` (fences, same-key coalescing, lane, shed) is shared by the single-event and batch paths; `SubmitMixedBatchFast` runs it per event but pushes the fast ring in 256-event chunks (a chunk the ring cannot take whole falls back to the single-event push/shed/wait rules); `SubmitShardedBatch` splits a batch per core (notes to their shard, everything else broadcast, per-core order preserved); the fast-owner path uses plain relaxed load+store instead of `lock xadd`/`lock inc` for the gate's per-key counters and the stats (the gate is documented race-tolerant). Player cost 18 -> 11 ns/msg on DYHTM (54M -> 62M msg/s at 4:47), note-on and event totals match the old path.
- 2026-10-16 `perf(v3): opt-in bend collapse (events.bend_collapse, live SetBendCollapse)` - Live profiling of DYHTM at the 4:23 crash showed ~85K pitch bends/s: every bend is a channel op that splits all voices of that channel into ~8-frame spans, so the release kernel ran on 25-frame average spans and blocks blew past 100%. Inside one compiler page a later bend on a channel now replaces the earlier one (same slot trick as the CC collapse). Crash window p99 load 252% -> 80%, late events 825K -> 0, early section 53% -> 33%. LOWERS ACCURACY (pitch steps can arrive up to a page late); default off, labelled that way in the configurator. Note-ons identical (1.24M both runs).
- 2026-10-16 `fix(v3): CC collapse recorded no slot offset` - the compiler-side CC collapse never set `record.offset`, so a superseded CC overwrote slot 0 of the page (whatever event was first) instead of its own earlier slot. Fixed together with the bend collapse.
- 2026-10-16 `perf(v3): opt-in fast chunk math (quality.fast_chunk_math, live SetFastChunkMath)` - AVX2 release-loop chunks take the gain ladder (decay^1..8 in double, rounded once) and the phase advance (`phase += step*8`) in closed form instead of two dependent chains of 8 ops. DYHTM jobs: early 4.21 -> 3.75 ms, late 3.09 -> 1.92 ms. Tolerance-equal: audio ~2.6e-5 vs the sequentially-rounded oracle (the less accurate side, its float phase drifts up to ~0.14 frames), so `TestFastChunkMathDifferential` re-runs the steal/launch/CC differentials at 1e-4 audio / 0.5-frame phase. Default off; the configurator lever says it lowers accuracy. Sample reads and interpolation untouched.
- 2026-10-16 `perf(v3): exact AVX2 kernel trims` - FilterTime8AVX2's unfiltered early-out is forced inline (out of line, every 8-frame chunk of an unfiltered voice paid a vzeroupper + call); sustained/release/transient chunks load contiguous samples + permutes instead of two gathers when the 8 phases span <16 samples (bit-exact, falls back to the gather); the release kernel renders its 1..7 frame remainder as a masked vector chunk (unfiltered, unrotated voices); whole-voice jobs are oversubscribed to 4 per pool thread (atomic job queue, merge order is per job index) with `RenderWorkerPool::GetJobCapacity()`. A few percent of job time together.
- 2026-10-16 note: with `synth.shards` N each shard's pool gets render_threads/N threads (16/6 = 2), so render_threads=20 measurably helps on a 20-thread CPU (early load 56 -> 44%, crash p99 294 -> 164%). `voices.retire_threshold` is the other big knob: -60 dB instead of -75.6 dB took the early section from 55% to 25% load.
- 2026-10-16 `feat(v3): long data is a raw MIDI byte stream (running status, packed events, split SysEx)` - midiOutLongMsg / SendDirectLongData(NoBuf) / native `send_midi_stream` (SVMS_CAP_MIDI_STREAM) feed one `MidiByteStreamParser`: channel messages batch through the fast ring, SysEx reassembles across buffers (used to be dropped unless one F0..F7 buffer). Also fixed SVMSAPI.h (didn't compile: undeclared procs, no mmsystem.h) and `TerminateKDMAPIStream` returning void vs the header's BOOL.
- 2026-10-16 `feat(v3): opt-in key-sharded engine (synth.shards) + single-producer ingest ring (events.fast_producer) + late-dispatch cap 2^19` - three things that landed together because they share the driver files. (1) `events.fast_producer`: the first submitting thread owns a private SPSC ring (plain stores, leased 256-sequence blocks, bulk batch push); bench model 98 M/s vs 29 M/s for the MPSC lane push with a live consumer, live player 22 -> 28 M ev/s; same-thread order is exact, only the tie-break against other threads' events can shift within a lease. (2) `synth.shards` (N>1): N headless Driver cores (own ring, compiler, scheduler, voice pool, renderer) behind the master; notes route by a mixing hash of (channel,key), everything else is broadcast; shard mixes are summed before the master's reverb/limiter, stats are summed. Bench 5.1x at 6 shards; max_voices and render_threads are split per core, per-channel limiter off, live levers reach the master core only. (3) kBlockDispatchSoftCap 1<<17 -> 1<<19 (owner's own edit; the cap was the 131072 events/block ceiling seen in the monitor). Both new features default off. ctest 18/18; XP build only fails on the same pre-existing test-TU symbols; not heard by me, owner is listening-testing.
- 2026-10-15 `feat(v3): opt-in steal reject (voices.steal_reject, default off)` - BASSMIDI-style: with a full pool and the Quality steal policy, a single-layer note-on whose steal score is strictly worse than the voice it would replace is dropped before launch (no ghost, no configure, victim keeps playing; the note-off pairing sees it exactly like a note stolen before it rendered). Bench (`--velocity-gain --steal-reject`, chopped-notes, random velocities, 8192 voices): 10M notes/s p50 178% -> 92%, 20M 319% -> 153%, but ~93-96% of the note-ons are dropped in that synthetic mix, so it is audible by design and needs a listening test before anyone turns it on. Uniform full-velocity material rejects 0 notes and the peek costs ~9% (p50 148% -> 161% at 10M); flag off is unchanged. New `TestStealReject`; ctest 18/18. Config JSON only: no configurator control, no live command.
- 2026-10-15 `perf(v3): producer path drops the submitted/accepted RMWs, keeps only the sequence counter` - SubmitShortMsgAtQpcCancellable did three locked RMWs per event; with a live consumer draining the queue one producer topped out at 15 M ev/s vs 19.4 M with just the sequence RMW (new `svms_v3_event_bench --contended`; padding the counters did nothing, the locks are the cost). `nextEventSequence_` is now 64-bit and doubles as `submitted`; `accepted = submitted - notAccepted`, and `notAcceptedAtomic_` only moves on the cold paths (cancel, shed, collapse drop). Event order/timing untouched. Windows ctest 18/18 (1 skipped as before); the XP build compiles the driver TUs but its test TU already failed on AVX2 symbols before this change. Not measured through the real Driver, only the bench model.
- 2026-10-15 `fix(v3): configurator stops calling ImFontAtlas::Build()` - RecreateFonts ran before the backend set RendererHasTextures, so imgui 1.92 logged an imgui-error on every start and DPI change (seen on Linux/SDL2). The atlas builds on demand now, so the call is gone. Windows + Linux configurators build; the Linux startup log is empty.
- 2026-10-15 `fix(v3): configurator says which controls a driver can't change live, and stops retrying them` - the Linux driver only registers master + limiter, so changing voices/reverb/correctness answered Unsupported, stayed pending, retried every 0.15 s and toasted "does not offer those controls". SocketRuntimeLink now applies what the driver has and names the rest ("voice count can't be changed live on this driver; save the configuration and restart it"); FlushLiveChanges drops an Unsupported change after one toast instead of retrying (shared code, so the Windows configurator gets the same no-retry). Windows build + ctest 18/18; Linux configurator built under WSL (Ubuntu 20.04, GCC 7), not exercised against a live driver.
- 2026-10-15 `fix(v3): carry the loop-wrap overflow into short (<8 frame) spans` - a long span can end with the loop wrap pending (phase >= loopEnd); RenderSustainedLoopSpan's short branch, the transient batch kernel and the primary sustained span then clamped that phase to loopStart and dropped the fractional overflow (0.126 samples, ~1e-2 amplitude in the gm.sf2 preset 5 repro). They now carry the overflow like the long path. Owner signed off the scalar output change; regression in svms_v3_realtime_block (fails before, 1e-7 after). Full ctest 18/18 incl. the vibrato golden; XP build not run. Other clamp sites (release/steal-tail/one-shot) left as they were.
- 2026-10-15 `feat(v3): Linux configurator (SDL2 + OpenGL3 + ImGui) driving the driver over the Control API` - Platform.h splits window/GPU/fonts/dialog-free OS bits out of ConfiguratorApp (Win32 + D3D11 stays as it was; Platform_SDL2.cpp is the Linux one). SocketRuntimeLink.h is a Linux svms::RuntimeLinkClient with the same surface, mapped onto RuntimeLiveStateV2/telemetry, so the pages are shared. Controls the configurator has no page for appear under Tools > Driver features ("unknown by this configurator version") straight from the driver schema. Windows-only pages (offline renderer, live recording, update check, WASAPI/ASIO pickers) are stubbed or replaced on Linux; the audio page gets an ALSA PCM box (audio.linux_device, which the old save path silently dropped). CI builds and packages svms_configurator. Needs libsdl2 + OpenGL at runtime.
- 2026-10-15 `perf(v3): Linux realtime loop renders each block in one batched RenderBlock call` - the KDMAPI audio loop rendered up to every event and dispatched it, re-entering RenderBlock per event (the offline path's own comment says that runs at a fraction of realtime at Black MIDI density). It now uses the same whole-voice batched path as Windows/offline, with Reset and SysEx messages as exact-frame barriers (SVMSRealtimeBlock.h). Side effects worth knowing: engine config (note merge, note-on collapse, re-excite) now actually applies on Linux - the old loop ignored it; the audio thread also gets flush-to-zero (Windows had it, Linux did not) and a best-effort SCHED_FIFO. Scalar parity with the old loop is exactly 0 on a note/CC/bend/sustain/Reset storm, 2.6x less render time at ~90 events/block. OPEN (found, not fixed - needs owner sign-off, boundary 2): the old loop and the batched path differ after some program changes only because the old loop splits blocks into many short spans. SVMSRenderKernels.cpp RenderSustainedLoopSpan has two wrap conventions: frameCount >= 8 wraps lazily at the top of the next iteration (a span can end with phase >= loopEnd), while the < 8 frame branch clamps a phase it finds past the region end to loopStart, dropping the fractional overflow (0.126 samples in the repro: key 83 of gm.sf2 preset 5, held 7 blocks, then three note-ons within 36 frames). Any long-span-then-short-span sequence on a looping voice loses up to one phase step at that wrap; the batched path rarely produces that sequence, which is why it matches a sequential float simulation exactly. Fix would be the same overflow carry at the entry of the short branch (and its RenderPrimaryVoiceSpan twins); it changes scalar output, so not done. Not run on Linux hardware yet.
- 2026-10-15 `feat(v3): Control API v1 for the Linux driver (self-describing NDJSON over a Unix socket)` - Linux has no shared-memory RuntimeLink, so the driver now hosts a socket at $XDG_RUNTIME_DIR/svms/svms-PID.sock. The hello message carries the whole schema (params, metrics, commands), so a configurator can show features it has never heard of instead of needing a protocol bump. Master volume + limiter are live today (applied at the next audio block); the registry is where reverb/decimation/etc. get added once the Linux runtime has them. Linux-only code is unbuilt on this machine; the registry test runs everywhere.
- 2026-10-15 `perf(v3): eight-voices-per-vector biquad for filtered sustained and release loops (bit-identical per voice)` — the SF2 low-pass was ~75% of a live voice (scalar in-order recurrence, 62 of 80 cycles per 8-sample chunk). New group kernels in SVMSRenderKernelsAVX2.cpp interpolate eight voices' chunks as before, 8x8-transpose so one vector holds the same time step of eight voices, run the identical biquad operations in the identical order (a0*2 as a0+a0, no FMA, same association), and transpose back. Each lane keeps its own frame cursor: a lane straddling its loop end steps scalar frames exactly like the per-voice kernel; any lane that cannot take a whole chunk (fewer than 8 frames left, or a release countdown/retire threshold not safe for 8 frames) ends the group and all lanes finish through the per-voice kernel on the remaining sub-span, which owns retirement. Dispatched from the sustained and release class kernels for spans of 8+ frames without rotation (bus mode too); runs of eight eligible filtered voices in handle order, everything else per-voice. Decimation blocks now also batch: PartitionMutedRows lays each job's slice out as mixed rows first (original relative order) then muted ones, so the kept voices form consecutive runs the batch kernels can take (muted rows add nothing, so mixed accumulation order is unchanged). `TestFilteredGroupKernel` (sustained + release, bus and shared plane, 3 spans, 27 voices incl. an unfiltered one splitting a group, retirements): per-voice phase, z1, z2, currentGain, release countdown and retirement list bit-identical; bus-mode planes bit-identical; shared-plane mix differs <= 1.2e-7 (lanes that drift apart can reorder one addition). All previous differentials unchanged. A/B vs --no-filter-groups with --filter-cents 13500 (AVX2): 1 thread cycles/sample sustained 10.1 -> 3.95, release 10.1 -> 4.5; 16 threads @30000 voices p50 sustained 43.3 -> 21.4, release 47.0 -> 23.6; decimated release @85000 (decimate-by-level) p50 19.3 -> 11.3. The remaining filter cost is the 8-step dependency chain (~1.5 cycles/sample); not done: interleaving two groups to hide it, transient-loop and one-shot classes, SSE2.
- 2026-10-15 `test(v3): bench --filter-cents models the SF2 low-pass; finding: the per-sample biquad is ~75% of a live voice` — every SF2 voice carries `PrepareVoiceLowPass` (enabled even at the default 13500 cents, the spec's wide-open value), and FilterTime8AVX2 runs it as a scalar in-order recurrence with SoA load/store of z1/z2 every sample. Single-thread cycles/sample @8000 voices, AVX2: sustained 2.41 -> 10.03 with the filter, release 3.23 -> 10.16 (the same at fc 13500 and 9000), so the old bench (no filter) understated live cost ~4x; that is why the bench showed ~9% p50 where the live monitor showed far more. The unfiltered kernel is uop-bound at ~19 cycles per 8-sample chunk and flat from 256 to 30000 voices (not cache bound); removing the 8-add phase chain saves 8%, a 16-sample-window permute fetch instead of the two gathers gained nothing (both tried and reverted). Decimation kept-voice count by tier (kept = active/step): 5k at 10k, 12.5k at 25k, 6.2k at 25001, 12.5k at 50k, 6.2k at 50001, 18.7k at 150k, 9.4k at 150001 — a sawtooth; with filters each 1000 kept voices is roughly 1.4% of the callback at 16 threads. Candidates (not built): bypass wide-open filters (audible, owner call), 8-voices-per-vector exact biquad (bit-identical to today), block-form IIR (tolerance-equal), budget-driven keep count instead of fixed tiers.
- 2026-10-15 `feat(v3): RENDER THREADS is a slider` — the ENGINE panel's 10-button key row (AUTO,1,2,4,8,12,16,24,32,64) is now a 0..64 slider (0 reads AUTO, ctrl+click to type), so any thread count is reachable and 'custom' no longer needs a special case. New `PanelSliderInt` widget. Compiles; relink needs the running Configurator closed.
- 2026-10-15 `feat(v3): cull muted tails defaults to 5 blocks; knob table is one row of four` — owner sign-off after the live A/B (no audible difference at 5-6 blocks, active voices down by more than half). Only matters once VOICE DECIMATION is on (still off by default). The Performance synth knobs are now a 4-column table with 54 px knobs instead of a wrapped second row. A saved config keeps its own `quality.decimation_cull_blocks`.
- 2026-10-15 `fix(v3): releasing-voice counter underflow when a voice releases and retires in one block` — the monitor's release count wrapped to ~4.2 billion and counted down. A voice released inside a block flips to Releasing in the worker, but its releasing-counter increment waits in the deferred-release list until block end; if the same voice retired in that block (short tails: the 10 ms minimum release is under a 2048-frame block, and the -55 dB floor makes tails shorter still), RetireVoice decremented a count that was never added and the finalize then skipped the Free row. The retire loop now finalizes a pending deferred release before retiring it. The launch differential now asserts the counter equals the number of Releasing rows each block (failed 6x before the fix). Also moved the CULL MUTED TAILS knob into the synth knob table (row 2 under the retire floor/per-key cap/ghost budget knobs). Owner A/B of the cull: no audible difference at 5-6 blocks, active voices down by more than half. ctest 16/16.
- 2026-10-15 `feat(v3): opt-in cull of tails that stay muted (CULL MUTED TAILS)` — with decimation on, a releasing voice that has been muted for K consecutive blocks is retired at frame 0 through the normal retirement list instead of advancing silently forever (cost ~80 cycles/block each, mostly cache misses). Runs are tracked per handle in the muted fast lane (row-local, parallel; two uint32 arrays, no extra serial pass). Config `quality.decimation_cull_blocks` (0 = off, default), live via `SetDecimation` param bits 8..15, Configurator knob 0..120 blocks, bench `--cull-muted N`. Held (non-releasing) voices are never culled. A/B (release @85k, 16 threads, AVX2, --decimation-by-level, 3 alternating rounds, p50): 10.3/10.1/9.9 -> 4.2/4.2/4.4 with --cull-muted 3, pool 85000 -> 5312 at the end (the bench never refills, so this is the end state, not a steady-state churn number). `TestMutedTailCull`: 40 releasing voices, step 4 -> 40,40,10,10,... with cull=2, untouched with cull=0. Audible only if the load later drops enough that a culled tail would have been mixed again; default off.
- 2026-10-15 `feat(v3): defaults -55 dB retire floor and decimate-by-level; Mixing Voices readout` — owner listening test: both clearly better. New defaults: voices.retire_threshold 0.00177828 (-55 dB; a saved config keeps its own value), quality.decimation_by_level true (decimation itself still off). The Monitor window shows `Mixing Voices: ~active/step` while decimating and the Configurator's render-load footer shows `~N mixing of M active` (muted voices still count as Active, Effective Voices only measures merge/coalescing). ctest 16/16.
- 2026-10-15 `feat(v3): opt-in decimation ranked by audibility (DECIMATE BY LEVEL)` — decimation can now mute the voices that are actually quietest (|envelope level| x stealOutputGain, the steal score's level term; pre-decay voices rank by target gain so a fresh attack is not muted) instead of the lowest velocity. Config `quality.decimation_by_level`, live via `SetDecimation` param 2 (1 stays velocity), Configurator lever on the Performance page, bench `--decimation-by-level`. Default off = unchanged. 8-bit key = float bits at 1/8-octave over 2^-32..1, histogram threshold as before; ties in the threshold bin are kept by an even Bresenham spread over the active list (keeping the first N piled every kept voice into the first one or two render jobs: jobs cycles tripled, p50 9.4 -> 22 at 85k, which was the whole cost). A/B vs velocity ranking (sustained @85k, 16 threads, AVX2, 5 alternating rounds, p50): 9.3-9.8 vs 9.3-9.6, i.e. the same. `TestWholeVoiceDecimationByLevel` (velocity falling with handle, level rising): energy kept 1.2% by velocity vs 45% by level of the undecimated mix. Oracle-vs-whole decimation errors unchanged. Not done: the frame-major oracle still ranks by velocity only.
- 2026-10-14 `feat(v3): RETIRE FLOOR now governs counted SF2 release tails (opt-in, default audio unchanged)` — found while preparing tail-policy options: live SF2 voices always carry an exact release sample count, and every retire test is `count==0 || (count==UINT32_MAX && gain<floor)`, so the RETIRE FLOOR knob never did anything for real voices. `FloorLimitedReleaseSamples` (SVMSEnvelope.h) now shortens the count at configure to the frame the tail crosses the floor (bound from the peak gain, so it never cuts above the floor); a floor at or below the default 0.00015 returns the count untouched = bit-identical. Applies to newly started notes, live via the existing knob. Expected tail length (and tail-voice CPU, by Little's law) vs the full count: -76 dB 95%, -66 82%, -60 75%, -50 62%, -40 50%, -30 37%. Not measurable in the bench (it re-seeds its pool every block, so tails never die there); the new `--retire-db` / `--release-ms` bench flags set the floor and a counted tail. `TestFloorLimitedReleaseSamples` pins the maths. ctest 16/16.
- 2026-10-14 `perf(v3): fast lane for muted carried-over rows, cheaper silent release advance` — measured first: a muted row costs ~80 cycles per block and is mostly cache-miss bound (decimated sustained 0.055 vs release 0.062 cycles/voice-sample at 85k), so there was little to batch. Two exact-semantics trims: the job loop sends a muted row with no start/release/op/excite/vibrato boundary straight to its single segment (skips the op window, bend-ratio powf, excite walk and batch probing), and SilentReleaseAdvance now decides survival with one logf + one expf (gain*decay^frames >= floor) and only computes the retire quotient for tails that actually die this block (was logf+logf+powf every block). Retire frames and gain unchanged within the existing ~1e-6 / 1-frame tolerance; muted voices only. A/B vs saved bench exe (release @85k, 16 threads, AVX2, --decimation, 5 alternating rounds, p50): 9.34 -> 8.94 (-4%); sustained 7.72 -> 7.57 (-2%, lane only). ctest 16/16.
- 2026-10-14 `perf(v3): closed-form silent advance for muted releasing voices` — live monitor at 85k voices showed release=82701 sustain=0 with decimation step 8 on and almost no CPU gain: a muted releasing row still ran a dependent per-frame gain recurrence (~8k cycles per voice per block). SilentReleaseAdvance now does the gain with one powf and finds the retire frame arithmetically (counted tails exact; threshold tails within 1 frame; gain ~1e-6 relative; muted voices only, nothing mixed). Falls back to the exact loop when decay is not in (0,1) or exactSilentAdvance is set. Decimation differentials unchanged (oracle-vs-whole 1e-7). A/B vs old bench exe (85k voices, 16 threads, AVX2, --decimation, 5 rounds, callback p50): release 50.8% -> 10.2%; chopped-notes 20.4% -> 21.0% (plan-bound, unchanged).
- 2026-10-14 `perf(v3): velocity histogram for the decimation mute set` — the per-block ranking of active voices was a full nth_element by (velocity desc, handle asc), ~35 cycles/voice serial; a 256-bin velocity histogram finds the threshold in one pass and only the voices sitting on it get ordered by handle. Same muted set (checked against the old ranking on random data with heavy ties, 25k-300k voices), so audio is identical; plan cost ~35 -> ~7-13 cycles/voice. A/B vs old bench exe (sustained, AVX2, --decimation, 5 rounds, cycles/voice-sample): 30k 0.122 -> 0.119, 100k 0.078 -> 0.073, 200k 0.053 -> 0.049. Not done on purpose: exactSilentAdvance for muted rows measures 2667 cycles per muted voice per block (a dependent add+wrap per frame) against ~270 wall cycles to just render the voice, so it would cost more than the decimation saves; the one-step drift is 0.04 samples per block on a voice that is not mixed.
- 2026-10-14 `perf(v3): cut scalar vibrato window overhead` — with a live LFO every event frame is a window for every voice, so per-window cost is the whole bill. Two exact changes: RenderWholeVoiceSegmentLfo ran a binary search over the block's window bounds for every window of every voice, now one search per segment and an iterator walk after; and the fallback-span telemetry counters (g_primarySpans, two per RenderPrimaryVoiceSpan) were locked fetch_adds, now plain load+store (an update can drop if two threads share a shard; it is telemetry only). powf stays: exp2f(x) differs from powf(2,x) on 132,594 of 2.16G inputs in [-4,4], so it is not a bit-exact swap. Output unchanged: scalar vibrato golden max still 8.38e-9, ctest 16/16. A/B vs old bench exe (scalar, chopped-notes @5000, 16000 notes/s, --cc-rate 4000 --cc-controller 1 --vibrato-lfo, 5 alternating rounds): 11.24 -> 6.15 cycles/voice-sample (no-vibrato baseline 1.46).
- 2026-10-14 `perf(v3): rotated release and transient rows ride the AVX2 time-chunk kernels` — the pair rotation now lives in one ChunkRotation helper shared by the sustained, release and transient time-chunk kernels; the scalar envelope gains[8] is untouched and rotation applies after the filter, the order RenderPrimaryVoiceSpan used for these classes before. Class kernels still refuse (before any mutation) for diffuse form-1 voices and for short spans (release <=4 frames, transient <=7), which go to the scalar fallback. Rotation differentials unchanged (AVX2 max 8.2e-7). A/B vs the old bench exe, 5 alternating rounds, medians (cycles/voice-sample): chopped-notes @5000 mode 1 2.46 -> 1.00, modes 2/4 2.43 -> 1.07; release @8192 3.76 -> 0.79-0.83; envelope @8192 3.9 -> 0.98-1.04; note-burst @5000 2.1 -> 0.86-0.93.
- 2026-10-14 `perf(v3): AVX2 time-chunk kernel takes Hilbert-pair rotated sustained rows` — RenderSustainedLoopFramesAVX2 now gathers x-hat next to x, lerps it, and rotates x*c - xhat*s between the lerp and the filter (same order as the scalar span). Static angles broadcast the exact c/s; sweeping angles (modes 2/4) use an 8-lane closed-form ladder of (dc+i*ds)^k per chunk instead of 8 dependent scalar steps (within the 2e-5 tolerance, not bit-equal to the scalar recurrence). Diffuse (form 1) and spans under 8 frames still go scalar; transient/release rotated rows still refuse. Rotation differentials AVX2 max <= 8.2e-7. A/B vs the old bench exe, 5 alternating rounds (cycles/voice-sample, medians): sustained @8192 mode 1 2.11 -> 0.70, modes 2/4 ~2.0 -> 1.30; chopped-notes @5000 mode 1 2.43 -> 1.36, modes 2/4 2.45 -> 1.85.
- 2026-10-14 `refactor(v3): drop the dead render-class block lists` — the span renderer is gone, so nothing walks the per-class handle pages any more. I kept the per-class counts (telemetry, tests) and the sustained renderGain bake in LinkRenderClass, and removed the pages, free stack, head/tail, mask, ForEachRenderClassBlock, the unused VoiceReleaseHook and GetMaxLaunchGroupSize. Linked state is now one byte per voice. Audio unchanged. A/B vs the old bench exe (chopped-notes 5M NPS, 5 rounds): 250 / 8192 / 65536 voices all within noise (p50 38.4 vs 38.9, 76.6 vs 76.8, 427.7 vs 431.5) — it was not a hot cost, just dead code.
- 2026-10-14 `refactor(v3): delete the span renderer, dense-tile kernel mode and stale path comments; decimation gets a live toggle` — `RenderBlockSparseRange`, `AdvanceVibratoSpan`, the coverage counters and `--force-path`/`--coverage` are gone (the bench keeps a stub `coverage` key); `RenderSpanContext::handleBase` and the AVX2 dense-tile loads went with the dense planner. The tests that needed the span renderer as a reference now use frozen output (`tests/SVMSVibratoGolden.h`, captured from it just before removal: scalar vibrato 8e-9, AVX2 vibrato 0.0175 as before) or the whole-voice rotation differentials; the two raw-dispatcher pair-rotation storm tests were dropped because those differentials cover modes 1/2/4. RenderScalar.h is 5749 -> 4146 lines. Decimation now has a live RuntimeLink command (`SetDecimation`) and a Configurator lever on the Performance page; `FULL CORRECTNESS` no longer claims to change live rendering. chopped-notes unchanged (median 0.5005 vs 0.506 before, interleaved x6). ctest 16/16.

- 2026-10-14 `feat(v3): opt-in voice decimation on the whole-voice renderer; the span renderer becomes test-only` — `quality.decimation` (default off, restart-applied; `RenderScalar::SetDecimationEnabled`). Above 10k/25k/50k/150k active voices (step 2/4/8/16) the quietest voices by velocity, ranked once at block start, stop mixing for the block; they still advance phase/envelope/retirement through the scalar no-mix span, and voices launched in the block always mix. Matches the frame-major oracle (2e-9 scalar, 1e-7 AVX2) with the audio untouched when off. Note the old span renderer never actually culled: it only sorted the voice list (the class kernels mixed everyone); the real cull existed only in the oracle, which is what this ports. Sustained 30000 voices, step 4: AVX2 0.414 -> 0.131 cycles/voice-sample (p50 20% -> 6%), scalar 1.056 -> 0.276 (47% -> 13%). The tier constants moved from 2k/8k/32k/131k to 10k/25k/50k/150k (`<=`). `RenderBlockSparseRange` and its helpers now compile only with SVMS_ENABLE_REFERENCE_RENDERER (tests/bench): production has the single whole-voice path, and a plan that cannot reserve storage skips the stretch (OOM only). Bench gets `--decimation`.

- 2026-10-14 `refactor(v3): delete the dense planner` — it only ever ran after a whole-voice refusal, and the forced-path bench showed it standing itself down after one block (7.5-13.4 cycles/voice-sample when it did run vs 0.4-1.0 on whole-voice). Gone: chunk-mask estimator, per-chunk snapshots/mutation plans, tile jobs, the shadow `VoiceSoA` and its dense-only reserve/copy modes, the adaptive gate and its pool-sized mark arrays, the dense reject/coverage counters, `TestDensePlannerOracleDifferential`, and the driver's 8192-voice storage step in the memory-budget search (the estimate is monotonic now). RenderScalar.h 5749 -> 4430 lines. `TestDenseProductionGateParity` stays as `TestWholeVoiceProductionGateParity`. chopped-notes 0.497 vs 0.504 median (base stage 4), sustained 8192 0.407 vs 0.420: unchanged within noise. ctest 16/16.

- 2026-10-14 `feat(v3): the whole-voice plan can no longer refuse a block` — storage is reserved up front (`RenderScalar::PrepareWholeVoice`, called at driver/standalone/offline init after the pool, threads and rotation mode are set; failure is an init error), the channel-op log is bounded by a 64k-op budget and a block that would exceed it is cut between event groups the same way barrier events cut it (no dispatch, same frames/order), and a render with no dispatcher ignores events like the sparse loop did instead of refusing. Matrix of 33 bench combos (3 backends x plain/CC1+LFO/lifecycle CCs/unmapped CC/bends/Reset/rotation 1,2,4/all-together/channel limiter): 100% whole-voice, zero refusals. `TestWholeVoiceOpBudgetSplitDifferential` (CC scenario with a 3-op budget) matches the oracle. Bench gets `--reset-rate`.

- 2026-10-14 `feat(v3): scalar/SSE2 vibrato rides the whole-voice renderer in the sparse loop's exact LFO windows` — vibrato no longer refuses the whole-voice plan on any backend. Scalar/SSE2 blocks split each voice's segment at the same window boundaries the sparse loop uses (every event frame plus every 64 frames inside a gap) and refresh the pitch increment with the shared `AdvanceVibratoSpanRow` (sparse now calls it too), so the output is the sparse loop's: scalar differential 8e-9 vs forced sparse. The sparse loop only samples 'any channel modulated' at block start; I kept that quirk so forced-scalar audio does not change. Rows without an LFO render as one segment. Bench (scalar, chopped-notes 5000, 16k notes/s + 4k CC1/s): voices without LFO 6.2 -> 1.3 cycles/voice-sample; voices with a 50-cent LFO ~12.7 -> ~11.3 (noisy, GTNH running). Bench gets `--vibrato-lfo`.

- 2026-10-14 `feat(v3): phase rotation (Analytic/Sweep/Random) rides the whole-voice renderer` — rotation no longer refuses the plan. Ghosts and ghost tails carry their rotation state (and adopted tails continue it), the Hilbert store reaches every whole-voice segment/tail, and an in-place steal successor gets the victim's angle advanced to the steal frame (it inherits its slot's state, the pre-pass only has the block-start copy; `AdvanceVoiceRotation`, square-and-multiply on the per-sample factor). Rotated chopped-notes @5000: 10.1 -> 2.3 cycles/voice-sample (p50 77% -> 16%), interleaved A/B x3. Steal/launch differentials in modes 1/2/4: ~1e-8 scalar and AVX2 vs the oracle; non-rotated chopped-notes unchanged (0.500 vs 0.508 median of 5). Bench gets `--rotation-mode`. Diffuse (mode 3) allpass memory is not advanced for steal successors yet, and is not in the differential.

- 2026-10-14 `feat(v3): split blocks around Reset/AllSoundOff/AllNotesOff instead of refusing the whole-voice path` — barrier events now dispatch at their exact frame between two whole-voice stretches, so a midiOutReset or GM reset no longer drops the block to sparse (forced sparse measured 8-18x slower). New `TestWholeVoiceResetBarrierDifferential` (scalar 3.7e-9, AVX2 2.4e-7 vs the oracle, state parity). Chopped-notes unchanged (0.484 vs 0.496 cycles/voice-sample, p50 3.4%). ctest 16/16.

- 2026-10-14 `chore(v3): path and refusal counters, bench --force-path` — first step of the single-renderer plan. I can now see how often whole-voice, dense and sparse run and why whole-voice refused, and force a path in the bench. Forced sparse is 8-18x slower than whole-voice on event material; dense stands itself down after one block. No audio change.

- 2026-10-14 feat(v3): note-on re-excite (note_on_collapse.reexcite, default off; SVMS_NOTE_REEXCITE; configurator RE-EXCITE REPEATS lever under the coalescing keys; RuntimeLink SetNoteOnReexcite 0x11D). With coalescing on, a collapsed repeat hit is kept at its exact timestamp as RenderEventType::NoteReExcite (ingress tags the message top byte) instead of being dropped. The audio thread restarts the key's live voices' envelopes (ExciteVoiceRow via VoiceManager::ReExciteKey: attack ramp of kReExciteAttackMs = 4 ms from the current level back to peak, decay restarts, sample phase untouched, no new voice) and launches normally if nothing sounds on the key. The first 48-frame ramp clicked, hence 4 ms. Ingress thinning (NoteOnCollapseGate::AllowReExcite): a hit within one ramp of the key's last kept hit (spawn or excite) is a no-op, so it is dropped before it costs an event: a key keeps at most 250 hits/s, which bounds the cost whatever the NPS. Whole-voice renderer support: the pre-pass records each hit per live voice (WholeVoiceExcite lists, generation-keyed by playIndex so ghosts and merged-fork twins hear the hits they took), applies the excite to the row so later steal scoring matches the oracle, and the workers restore the block-start envelope and replay each hit at its exact frame, skipping hits inside a running ramp. Dense planning still refuses blocks carrying one. TestReExciteKey (+ gate/compile checks) and TestReExciteRenderDifferential (roomy pool, two-voice steal/ghost script, merged forks): oracle vs whole-voice 9e-10 scalar / 1.6e-7 AVX2, state parity and steal counts equal; ctest 16/16. Bench: chopped-notes @5000 voices p50 3.4% -> 5.2% with --reexcite-rate 5000000 (thinned to ~2.1k hits per 2048-frame block); unthinned, 1M hits/s cost 60% p50, which is why the gate thins. Offline StandaloneSynth mirrors it. Not heard on a real file by me, XP build not run.
- 2026-10-14 feat(v3): effective-voice counter also weights live voices by note-on coalescing's per-key spawn stack (NoteOnCollapseGate::SpawnWeight, census scan in RenderCallback); shown in the performance tile, MIDI line and diagnostics window. Tried an N^e merged-stack level (sqrt etc.) by ear on DYHTM and removed it: it did not close the gap to coalescing, which mostly removes repeated attacks, not stack loudness.
- 2026-10-14 feat(v3): exact note-on merging (events.note_merge, default off; configurator EXACT NOTE MERGE lever; SVMS_NOTE_MERGE env for tools). Same channel/key/velocity/frame note-ons fold into one voice at N times the gain (SVMSNoteMerge.h, run on each sorted compiler page and on offline blocks); each note-off releases one unit and a partial release forks a releasing twin (VoiceManager::ForkReleasedUnits), because 60-74% of duplicate copies in the big merges end at different ticks. Separate from the 20 ms NOTE-ON COALESCING gate, which is untouched. Effective-voices counter added (telemetry, runtime link, configurator, census). Dense planner stands down while merged voices exist. Merged vs unmerged renders match to 4e-8 (scalar) / 2e-6 (AVX2) in TestMergedNoteExactness; ctest 16/16. Not done: XP build, full build_v3 relink (configurator held winmm.dll), merged-vs-unmerged WAV compare on real files, bench A/B numbers.
- 2026-10-11 feat(v3): SVMSConfig.cpp now compiles on Linux (XDG ~/.config/SuperVirtualMIDISynth/config.json, UTF-32 wchar shim, dladdr module dir, Filesystem-TS fallbacks for GCC 7), replacing the StandaloneSynth stopgap; Linux and Windows read the same config schema. Linux CI is green again (7/7). In-process mutex only on Linux for now (no cross-process lock).
- 2026-10-11 fix(v3): Linux build: StandaloneSynth called EngineConfig::Load() (a7df65d) but Linux doesn't compile SVMSConfig.cpp, so svmsd/svms_v3_render failed to link and the Linux CI job had been red. Windows keeps reading the config; Linux uses the default (note-on collapse off). Proper fix is making SVMSConfig.cpp portable.
- 2026-10-11 feat(v3): nested-backend guard (api.allow_nested). An SVMS instance started inside another synth host (the OmniMIDIv2 plugin sets SVMS_NESTED=1) now ignores api.backend instead of routing events back out and looping; ALLOW NESTED lever in the configurator overrides it. Found while running SVMS as an OMv2 plugin: with the override on, SVMS -> OMv2 -> SVMS hangs. ctest 16/16; XP build not run.
- 2026-10-11 fix(v3): configurator alignment pass: RESTART tags and lever labels centred on their line, meter bars centred in their frame, master strip spacing.
- 2026-10-11 fix(v3): configurator "Refined" style renamed "Boring"; rack key text/lamp vertically balanced.
- 2026-10-11 fix(v3): configurator master strip is taller so the MASTER knob label is no longer cut off by the chain strip.
- 2026-10-11 feat(v3): configurator Style option (Rack / Refined) and key spacing fix
  Feedback was that the rack look reads like an old plugin, and that the key
  labels had no room (lamp and text were stacked tight). Rack keys are now
  taller and wider with the lamp clear of the label. New theme option Style:
  Rack (unchanged look) or Refined (flat panels, segmented key tracks, pill
  toggles, quiet restart chips, sentence-case sans labels via StyleText, no
  screws or tick marks). Stored as "style" in the theme file; old themes load
  as Rack. RESTART tags follow the accent instead of a fixed amber.

- 2026-10-10 feat(v3): SoundFont panels, Ctrl+click knob entry, window minimum size
  SoundFont is now a current-font LCD with Browse/Clear/Load now keys, a folder
  browser beside it, a bank stack with Up/Down/Remove keys and a routes panel.
  Knobs take a typed value on Ctrl+click (KnobState::inverseFn maps dB/kHz
  displays back to the stored value; Enter commits, Esc cancels, one live push
  on commit). WM_GETMINMAXINFO enforces 1100x660 client so layouts never clip.
  LED ladders centre their leftover space so top and bottom padding match,
  which fixes the gain reduction bar looking upside down.

- 2026-10-10 feat(v3): synth, out and MIDI stages recomposed as panels
  Even with the new chain the stage pages were still long label/control row
  lists. SYNTH, OUT and MIDI are now rack panels: big LCD readouts (voice
  ceiling, latency), key groups for choices, knobs for continuous values
  (retire floor, per-key cap, ghost budget, velocity), levers for switches,
  LCD number fields. New PanelKeys/PanelKnob/PanelLever/PanelLcdInt widgets;
  RESTART tags sit next to the setting they apply to. The limiter gain
  reduction bar is slim and reads empty at 0 dB. Same commands, nothing removed.

- 2026-10-10 feat(v3): configurator is a signal chain, not a sidebar of pages
  Someone said it looked like the Realtek console, and structurally it did: a
  sidebar of pages full of label/control rows. The sidebar is gone. A master
  strip (volume, output meter, voices, CPU) is always visible, and a clickable
  chain HOME / MIDI -> SYNTH -> REVERB -> LIMITER -> OUT replaces it, with a
  TOOLS menu for the pages you open rarely. Home is a four-module front panel
  (live knobs, OPEN jumps to a stage); Esc goes back to Home. The footer is one
  aligned row with flat disabled keys. No setting was removed.

- 2026-10-10 feat(v3): rack-style configurator (theme, widgets, page layouts)
  People said the configurator was ugly, so I redid it as a hardware rack:
  warm chassis, raised keys with lamps, LCD readouts, screwed panels, mono
  captions. The theme still comes from the one colour wheel + strength slider
  (ApplyRackPalette derives every tone; saved theme files stay compatible) and
  gained Amber/Phosphor/Ice/Magenta/Mono presets. SectionHeader now opens a
  panel automatically and settings labels show a one-line description, so every
  page picked it up; Audio, Reverb (LCD decay trace), Limiter (LED ladders,
  LCD history) and Per-Channel Limiter got real layouts. No setting was removed.

- 2026-10-10 perf(v3): make the event queue storage lazy
  My config had ring_capacity 20M and the synth sat at 1.4 GB idle: the
  ingress cells, compiled pages and legacy scheduler scratch were all
  zero-filled at startup. Ingress cells now store sequence minus index so
  zeroed OS pages are a valid empty ring, pages only init their header, and
  the scheduler scratch is raw storage. Structures at 20M: 1192 MB -> 13 MB;
  engine peak working set 75 MB. Lane bases are staggered by 64 B multiples
  because page-aligned lanes 4K-alias in the producer (-25%); event bench
  producer 218 -> 202 M/s, compiler and callback unchanged.

- 2026-10-10 perf(v3): stop render workers sharing hot counter cache lines
  The whole-voice segment counters were plain members of the shared
  RenderScalar, bumped by all 16 workers on every segment, and the job
  retirement counts were packed uint32s. Under a bend riser (each bend splits
  every voice of its channel into a new segment) the cache-line ping-pong
  cost ~2000 cycles per segment and was most of the render. Segment stats are
  now per job (own cache line, no atomics), retirement counts are 64 bytes
  apart, and the global primary-span counters are sharded per thread.
  chopped-notes 480 frames, p50: 32k voices @ 50k bends/s 499% -> 76%,
  @ 150k 1470% -> 135%; 131k voices @ 50k 1935% -> 260%. No audio change.
- 2026-10-10 test(v3): bench --bend-rate; trace render classes and wv jobs mode
  Pitch-bend risers are what blow up high-polyphony renders: every bend is an
  exact-frame op on each voice of its channel, so whole-voice job cost grows
  with voices x bends (chopped-notes, 32k voices, 480 frames: 35% p50 at 0
  bends/s, 103% at 10k, 499% at 50k, 1470% at 150k; DYHTM Community Merge
  4:13-4:24 runs 42k-188k bends/s). The callback trace now carries per-class
  voice counts, whole-voice kernel vs scalar-fallback segment frames and
  whether the jobs stage ran serially.
- 2026-10-10 fix(v3): the 131072-event block cap only limits late events
  It counted every examined event, on-time ones included, so a sustained
  stream above ~13M events/s (10 ms callbacks) went late by design with the
  render thread at 20-30%: DYHTM Community Merge 4:25-4:46 had 8-10M late
  events/s and up to 90 ms lateness, now ~0. It now counts only late events
  that are dispatched, so obsolete note-ons skipped during recovery no longer
  hold a backlog behind. max_events_per_block still bounds the whole block.
  The trace record gains callback and whole-voice plan/jobs/post TSC cycles.
- 2026-10-09 feat(v3): per-callback trace ring on the native API
  enable_callback_trace / read_callback_trace (SVMS_CAP_CALLBACK_TRACE) record
  every audio callback: schedule/render/post time, events admitted, late
  clamps and their worst lateness, recovery-skipped frames, ingress/compiled/
  scheduled backlog, voices, render path, and cumulative note-ons, steals and
  producer backpressure waits (now timed on the slow path). 8192-record ring,
  allocated on first enable; off = two branch-not-taken loads per callback.
- 2026-10-07 fix(v3): build the canonical engine as C++17. Span/Path shims in
  Engine/CanonicalCompat.h replace std::span and std::filesystem (GCC 7 has only
  the Filesystem TS); atomic wait falls back to a mutex + condition variable.
- 2026-10-07 ci(v3): save the Linux ccache even when the job fails; retry apt
  actions/cache only saves on success, so a red Linux job never warmed its
  cache. Restore and save are separate steps now (save runs always), the
  container makes its root-owned cache readable from an exit trap, and apt
  retries downloads (the last run died on an archive.ubuntu.com timeout).
- 2026-10-07 fix(v3): restore the Linux build (POSIX worker pool, GCC 7 filesystem)
  The POSIX worker pool had drifted from the Windows one: old indexed-job
  signature, no bus planes, kernel refusals silently dropped, and span jobs
  writing retirements into one shared array from every thread. It now mirrors
  the Windows job semantics line for line (futex sync kept). SVMSSoundFont.cpp
  falls back to the Filesystem TS on GCC 7 (Ubuntu 18.04 has no <filesystem>).
- 2026-10-07 fix(v3): make per-voice phase rotation exactly analytic
  Two things kept rotation from being a pure phase shift. The allpass
  "quadrature splitter" (still used by Random) was never in quadrature: its
  branches were 7-170 degrees apart, so each voice got an angle-dependent
  filter. And the Hilbert companion was built per whole sample slice, so over
  a loop it stepped at every wrap (a click at the loop rate) and the voice's
  level depended on the angle. The new SVMSHilbertPair.h builds the loop as a
  periodic transform, crossfades attack/tail into it and guards one-shots;
  modes 1/2/4 always use the exact pair (Random = per-voice sweep rate and
  direction), and the splitter is gone. Correctness suite green again
  (was red since ce65158); new loop tests fail on the old builder.
- 2026-10-07 feat(v3): canonical engine phase 2 (WIP) + launch-hot VoiceRow
  Unfinished work pushed as-is. src/V3/Engine/ holds the canonical engine,
  wired only into svms_v3_render behind a legacy|canonical selector (see
  CANONICAL_ENGINE_MIGRATION.md). Also in here: the V3 voice pool packs its
  launch bookkeeping fields into one 64-byte VoiceRow per voice; ghost copies
  copy the row. Correctness suite unchanged (only the known Hilbert failure);
  not benchmarked yet.
- 2026-10-07 ci(v3): warm compiler caches for the Windows and Linux builds
  Windows CI runs build_v3.bat under sccache (GitHub Actions cache backend);
  CMake sees the launcher and switches to /Z7 + no driver PCH so every object
  is cacheable. The Linux (Ubuntu 18.04 container) job uses ccache with its
  directory saved by actions/cache. The first run fills the caches.
- 2026-10-07 build(v3): precompile SVMSDriverInternal.h for the driver sources
  The 10 driver translation units each re-parsed the big engine headers
  (~1.2 s apiece). A PCH (SVMS_DRIVER_PCH, driver files only; kernels/ASIO/etc.
  skip it) brings a header touch to 5.7 s (was 10.5) and a single-file edit
  to ~2 s. Under a compiler-cache launcher the PCH defaults off and /Zi becomes
  /Z7, so sccache can cache everything in CI.
- 2026-10-07 build(v3): compile the engine once for winmm.dll and SVMSAPI.dll
  Both DLLs used to compile every engine source separately. They now link one
  `svms_engine` OBJECT library; only SVMSFrontWinMM.cpp is still built per DLL,
  because svmsapi compiles it with _WINMM_. Export and import tables of both
  DLLs are identical. Touching SVMSDriverInternal.h rebuilds in 10.5 s (was 23 s).
- 2026-10-07 fix(v3): value-initialize SF2Data with () instead of {}
  `new SF2Data{}` aggregate-initializes 65536 SFSampleRegions that have member
  initializers, and cl.exe pays ~3 s plus a lot of memory for it in every TU
  that includes SVMSStandaloneSynth.h (it ran a full-parallel build out of
  RAM). `new SF2Data()` gives the same zeros-plus-defaults object.
- 2026-10-07 refactor(v3): split SVMSDriver.cpp by area
  The 10k-line driver is now SVMSDriverInternal.h (class Driver + shared
  helpers) plus SVMSDriver/RuntimeLink/SoundFont/Backend/Ingress/Render.cpp
  and the front ends SVMSFrontWinMM/Native/KDMAPI/Bass.cpp + SVMSDllMain.cpp.
  Code moved verbatim; the only edits are linkage (shared file-scope statics
  became inline, four native offline functions lost `static` because the BASS
  shim calls them). Export tables and test output are identical. A one-area
  edit rebuilds in ~3-4 s instead of ~11.5 s; touching the internal header
  costs ~23 s at -j 2. SVMSNativeOffline.h is included only by the native
  front end: its StandaloneSynth `new SF2Data{}` is expensive for cl.exe and
  ran a full-parallel build out of memory when every file included it.
- 2026-10-07 refactor(v3): split RenderEvent out of SVMSRenderScalar.h
  SVMSEventScheduler.h included the whole renderer (and with it the voice
  manager) just for `RenderEvent`. The event-pipeline headers now include the
  40-line SVMSRenderEvent.h instead. No code changes.
- 2026-10-07 refactor(v3): move the driver reverb into SVMSReverb.h
  First cut of the monolith split. `ReverbState` was 1,460 self-contained lines
  in the middle of SVMSDriver.cpp; it moved verbatim. The DLL export tables
  and test output are identical before and after.
- 2026-10-07 perf(v3): trim whole-voice plan loop (O(1) release-op drop, slim ghost copy, same-frame note batching)
  The serial plan is 93-100% of block time at multi-M NPS. I made the
  deferred-release op drop O(1) (per-handle index), copy only the fields
  ghosts actually read on each steal, and hand same-frame note runs to the
  batch dispatcher in one call so the driver's exact-frame batching engages
  (ordinal semantics unchanged). I also fixed a bench artifact
  (8 VoiceConfiguration ctors per note-on) and added the wv_cycles split.
  Whole-voice test output is byte-identical. p50 250v/15M 152 -> 118,
  8192v/5M 124 -> 112; most of that is the bench fix. The remaining wall is
  victim-row cache misses in LaunchVoiceGroup.

- 2026-09-16 fix(v3): honor SIMD worker fallback for phase-rotated voices
  The render worker pool now preserves the documented class-kernel contract:
  when AVX2/SSE2 refuses a complete job without mutation, the matching scalar
  class kernel consumes that same private worker slice immediately. This fixes
  rotation-enabled transient/release batches being silently dropped, which
  froze envelope/release progression and left voices stuck active. Random mode
  also keeps its intended jittered allpass+sweep path instead of collapsing
  into the same exact-Hilbert form as Sweep.


- 2026-09-16 fix(v3): make Analytic phase rotation use the real Hilbert pair
  I build the analytic companion with every realtime SoundFont bundle so live
  mode switches cannot silently fall back to the quadrature approximation,
  wire the same pair through standalone/offline rendering, and order filtered
  rotated voices as Hilbert rotation -> low-pass so the unfiltered companion
  is never combined with a differently filtered real component. Rotation still
  refuses whole-voice/dense paths where their state model cannot represent it;
  those paths fall back for correctness rather than approximating the DSP.

- 2026-09-15 feat(v3): add initial SFZ instrument support
  I compile external WAV-backed SFZ regions into the existing V3 sample and
  region pipeline, including inheritance, mapping, tuning, gain/pan, loops,
  amp envelopes, group/off_by, recursive configurator discovery, and tests.

- 2026-09-15 feat(v3): honor SF2 exclusive classes on note launch
  I track active exclusive-class voices by MIDI channel and choke the prior
  complete play group at the exact note-on frame without self-choking layers.

- 2026-09-15 perf(v3): batch matching whole-voice render segments
  I dispatch consecutive voices with the same exact start frame and render
  class through one class-kernel call when no release, row-op, or vibrato
  boundary splits them, preserving handle order and scalar mix order.

- 2026-09-14 feat(v3): play extended 256-key MIDI and add selectable 31EDO tuning
  I preserve full-byte note identities through playback and offline decoding,
  extend top SoundFont zones, and keep normal tuning unchanged. 31EDO is
  restart-applied (key 155 = middle C); percussion keeps its instrument keys.
- 2026-09-14 fix(v3): reject inaccessible SysEx buffers before parsing
  I copy caller buffers safely so Ziggy cannot crash the DLL by passing -1
  as a long-message data pointer; KDMAPI reports an invalid parameter.


- 2026-09-14 fix(v3): restore the channel activity bars after the label-width
  alignment fix pushed the right-column meters outside their table cells.
  Per-channel gain reduction now ramps down over 0.5 ms instead of jumping
  in one sample; the master limiter catches the brief attack overshoot, so
  threshold crossings stop clicking without adding lookahead latency.

- 2026-09-14 fix(v3): clear capacity-strided channel buses per plane
  instead of as a packed active-frame prefix. Variable-size audio callbacks
  could otherwise re-sum stale samples from earlier blocks, sounding like
  stuck notes and compounding distortion. The live threshold glide now
  advances once per frame and is shared consistently by all 16 channels;
  channel 9's configurator meter reserves the same two-digit label width as
  channels 10-16 so its bar aligns with the column.

- 2026-09-14 fix(v3): channel-limiter live updates never reached the engine
  when no grouped change was pending — FlushLiveChanges sent ApplyLiveConfig
  with an empty group mask (driver rejected "Invalid argument - empty group
  mask") and the dedicated SetChannelLimiter command only ran behind a
  success return. ApplyLiveConfig is now skipped entirely when the grouped
  mask is zero, so per-channel-limiter-only changes flush on their own
  command. Same screenshots: the fixed 24 dB GR scale pegged half the
  channel grid full — Black MIDI buses peak 20-40 dB above full scale, so
  the grid now steps a shared display max (24/48/96/192) off the worst
  channel with a scale caption, mirroring the limiter page.

- 2026-09-14 fix(v3): Per-Channel Limiter page cleanup from the owner's
  screenshots — the enable toggle rendered its raw "##cl_enabled_switch"
  ImGui id (ToggleSwitch draws its label; pass "ENABLED"), the channel
  activity grid went from a sparse 4x4 vertical-bar table to a compact
  2-column x 8-row layout with tight horizontal GR bars, and the topology
  description paragraph is gone (release knob gains a tooltip instead).

- 2026-09-13 feat(v3): per-MIDI-channel limiter (opt-in, default OFF) — 16
  stereo channel buses limited independently, then summed into the master
  chain. Purely POST: RenderBlock gains optional channelBusLeft/Right plane
  tables; when the feature is off nothing runs and output is bit-identical
  (boundary #2 honoured by construction). The limiter itself is classic
  zero-latency (SVMSChannelLimiter.h): per-channel peak envelope with
  instant attack + one-pole release toward the running peak (mirror of the
  master Classic detector), 4 dB soft knee, gain applied from the envelope
  of the same sample — above the knee a bus never exceeds the threshold
  exactly, inside the knee the worst case is threshold*knee. Deliberately
  NOT predictive: 16 lookahead lines would add latency to guard transients
  the master limiter already owns; this stage is about one runaway channel
  eating everyone else's headroom. Plumbing: RenderSpanContext grew
  channelBusLeft/Right (aggregate initialisers value-init them null, legacy
  sites untouched); class kernels select the voice's plane per voice (a
  voice's MIDI channel is fixed for life — one pointer select per span, no
  per-frame scatter); worker pool gained lazy per-job bus planes with a
  per-channel deterministic merge; dense tiles re-batch per job in bus mode
  (390 private 16-plane buffers for a 100k pool would be absurd); the AVX2
  voice-batched 1-7-frame short kernels refuse in bus mode (eight lanes =
  eight channels into one frame position — boundary #7 refuse, scalar
  per-voice fallback takes it); SSE2/scalar short batch write planes
  directly. Driver allocates the buses alongside the mix buffers (never in
  the callback), limits+sums after RenderBlock, before reverb/master
  limiter. Live via a dedicated SetChannelLimiter wire command
  ("enabled;threshold;releaseMs" in the command text area — the live-state
  V2 struct is ABI-pinned) with block-boundary adoption and threshold
  glide; telemetry grew prefix-compatibly 512 -> 704 bytes (per-channel GR
  + pre-limit peak, structSize-driven offsets handle both sizes — the ABI
  smoke test pins the append). Configurator: new Effects > Per-Channel
  Limiter page (toggle, threshold/release knobs, 4x4 channel GR grid),
  JSON section channel_limiter. Tests: TestPerChannelLimiterDifferential —
  limiter unit properties (ceiling, exact quiet passthrough, step response,
  release), engine-vs-oracle bus parity (scalar 3.7e-9; AVX2 worst 2.3e-3 =
  the documented steal-storm victim-flip class, budget 1e-2), both bus
  paths (whole-voice via even blocks, dense/sparse via AllSoundOff-refused
  odd blocks), direct-mix continuity after bus blocks. ctest 15/15, XP
  13/13. Bench (AVX2, 6 threads, 2048 frames): sustained @8192 0.493 ->
  0.519, note-burst @5000 0.568 -> 0.623, chopped-notes @5000 0.605 ->
  0.683 cycles/voice-sample with the feature ON; OFF matches baseline.

- 2026-09-13 perf(v3): CC1 vibrato joins the whole-voice fast path. The old
  "any channel modDepth > 0 -> refuse whole-voice + dense, cap sparse spans
  at 64 frames" gate cost 9.2x on modulated material (chopped-notes + CC1:
  5.25 vs 0.57 cycles/voice-sample) because ONE mod-wheel touch poisoned
  every subsequent block. Now the per-row AVX2 kernels rebuild the 64-frame
  LFO control window internally (scalar advance-then-use triangle +
  degree-5 exp2 Taylor instead of powf, windows never straddle a fast
  chunk), and CC1/CC121/channel-pressure report post-rebuild depth through
  a new vibrato row-op (kind 2) that the worker items track alongside bend
  ratio cursors. Whole-voice takes vibrato only on the AVX2 backend —
  scalar/SSE2 keep the legacy path bit-identical (boundary #2), and the
  sparse path keeps AdvanceVibratoSpan as the reference. Whole-voice-vs-
  sparse waveform parity carries the documented control-rate drift (window
  boundaries differ): TestWholeVoiceVibratoDifferential measures 1.7e-2
  saturating over 8 blocks, budget 3e-2. Bench: CC1 5.25 -> 0.97 (1.4x over
  baseline). Also: per-channel vibrato relevance flags keep non-vibrato
  blocks at their pre-change cost, and the whole-voice plan now accepts
  unmapped CCs as audio-exact no-ops (they have no engine effect).
- 2026-09-13 fix(v3): per-segment rdtsc profiling in RenderWholeVoiceSegment
  gated behind SVMS_WV_SEGMENT_PROFILE (default off). The six rdtsc per
  segment added by the OneShot-kernel telemetry cost ~+8% on note-only
  material and ~+25-35% on op-fragmented blocks (every CC/bend row-op splits
  timelines into more segments). Bench dispatch fixes alongside: pitch bends
  now route through ApplyChannelBendRatio like Driver::HandlePitchBend (the
  old inline rewrite bypassed the whole-voice bend-op hook, so bend-heavy
  bench numbers were fictional), and --cc-controller 1 emits nonzero values
  so the vibrato-gate A/B is honest. Measured @5000 voices chopped-notes:
  baseline 0.617 -> 0.570, +CC7 1.261 -> 0.816, mixed-events 4.156 -> 3.144;
  the CC1 vibrato gate (legacy sparse fallback) measures 5.253 = the 9.2x
  cliff the owner flagged.
- 2026-09-12 feat(v3): BASS shim surface extended to the full genuine
  BASSMIDI export list (40 names). BASS_MIDI_StreamEvent (singular — the
  API PFA-1.1.0viz imports) is implemented for real: translates via the
  verified type table, anchors at the consumption cursor, and round-trips
  through a new per-stream (channel,type)->param cache that
  BASS_MIDI_StreamGetEvent queries. FontFlags/SetVolume/GetVolume,
  StreamLoadSamples, StreamSetFilter, FontCompact/Unload are functional
  no-ops; file/URL/user-stream creation, event/mark/preset queries, and the
  BASS_MIDI_In* surface are graceful-failure stubs so statically-importing
  hosts load (file-based MIDI creation would need a parser — the BPFA/PGFA
  family uses StreamCreate + StreamEvents per the decompiles). Forwarder
  and both /EXPORT lists updated; probe gained singular-API tests.

- 2026-09-12 feat(v3): offline/standalone synth mirrors the realtime
  same-key note-on coalescing (SVMSNoteOnCollapse gate in the audio-frame
  domain — same fixed 20 ms window, same note_on_collapse.threshold config
  knob, same velocity stacking of collapsed hits, CC120/123 and reset clear
  the gate). Optional by construction: threshold < 2 (the default) spawns
  every note-on. Live-pump bench, 800k notes/s hammered on 4 keys: 0.29x
  realtime with coalescing off (6.4M launches, 12.8M steals) vs 2.45x with
  threshold 32 (208k launches, 411k steals) — 8.4x. This is the shape the
  owner's realtime path already handles via coalescing; prerender now
  matches it.
- 2026-09-12 feat(v3): offline telemetry gained dispatch_coalesced (struct
  128 -> 136 bytes, ABI asserts updated); the shim's profile/final log
  lines report it.

- 2026-09-12 fix(v3): BASS shim per-event log flood removed — StreamEvents
  logged a full file open/write/close line per call, and realtime pumps
  submit one call per MIDI event (the owner's %TEMP% log hit 160 MB /
  2.4M lines from a single Kiva session). Per-event lines are gone
  (failures only, rate-limited), GetData profile lines now every 1024th
  pull, BassLog is throttled to ~1 line/second with a forced variant for
  rare lifecycle lines (FontInit/StreamCreate/final stats), and the file
  self-truncates past 4 MB. Live-pump bench (one StreamEvents call per
  event, 10 ms pulls): 100k notes/s @ 200 poly = 4.6x realtime, 16k @ 88
  keys = 8.4x.
- 2026-09-12 fix(v3): realtime audio-callback census lines
  ([SVMS] sched/flow/pool/planRefuse, every 64 callbacks) are compile-time
  OFF by default (SVMS_AUDIO_CENSUS=1 to re-enable) — the only recurring
  debug-stream traffic in the winmm path, and the DebugView destabilizer in
  OmniMIDI's presence. One-time init/failure messages are unchanged.

- 2026-09-12 perf(v3): StandaloneSynth note-ons go through the production
  LaunchVoiceGroup transaction (setups built up front, batched victim
  selection, sibling slots feed layers) with steal batching enabled, instead
  of a per-region AllocateVoiceOrSteal + ConfigureVoice loop. Same victims
  and configuration; sustained workload 50x -> 80x realtime, note-on
  dispatch cheaper under pool pressure.
- 2026-09-12 feat(v3): offline telemetry extended (struct_size-gated, 64 ->
  128 bytes) with the render-path bitmask and the standalone synth's
  dispatch-phase cycle profile (note-on/off, region resolve, alloc,
  configure, control, render total); BASS shim logs it per stream (final)
  and every 64th pull. SVMS_BASS_THREADS env var pins the offline render
  thread count for parallel-scaling diagnostics (1 thread = 22.4Gcyc vs
  auto 3.9Gcyc wall on the chopped load — the whole-voice render scales).

- 2026-09-12 perf(v3): BASSMIDI offline sessions render through the
  production RenderBlock machinery. The native offline path dispatched each
  event individually and rendered a span between events — at Black MIDI
  densities a full RenderBlock call every few frames (0.08x realtime on a
  chopped-notes load). Events now convert to RenderEvents and hand the whole
  block to RenderBlock (whole-voice/dense/sparse + worker pool), via a
  production EventDispatcher wired into StandaloneSynth whose bend routing
  goes through VoiceManager::ApplyChannelBendRatio (pre-pass-aware).
  Throughput: 0.08x -> ~10.4x realtime on chopped-notes @16k note rate;
  sustained ~49x; probe peaks bit-identical to the old path.
- 2026-09-12 perf(v3): BASS shim render-ahead cache — GetData refills in
  max_block_frames engine chunks and serves pull-sized requests, so
  small-pull callers (CSCore ISampleSource reads) match big-pull throughput.
  Only streams proven batch (a TIME-anchored event seen, no positionless
  event) render ahead; realtime pumps keep exact cursor anchoring — a deep
  cache would anchor their positionless events in already-rendered audio
  (caught by the send-after-pull probe). GetPosition/IsActive/available now
  report the served cursor.
- 2026-09-12 feat(v3): StandaloneSynth gained RenderWithEvents (event-batched
  RenderBlock with correctnessMode=false, i.e. production decimation tiers)
  plus a static DispatchRenderEvent; offline sessions hold back events at
  frame_offset == frameCount and apply them after the block (outside tested
  RenderBlock dispatch territory).

- 2026-09-11 fix(v3): BASS shim GetData BASS_DATA_FLOAT corrected to
  0x40000000 (reflected Bass.Net) — the shim stripped 0x400, so Kiva's
  1 MB pulls arrived still flagged and were treated as ~1 GB requests,
  rendering frames far past the caller's buffer. This overrun is the
  strongest suspect for the owner's "trash audio" prerender report.
- 2026-09-11 fix(v3): BASSMIDI struct-event type table corrected against
  Bass.Net's BASSMIDIEvent reflection: REVERB=23/CHORUS=24 (16/17 are
  SOUNDOFF/RESET — the old table played SoundOff/Reset as reverb/chorus
  CCs). Added SOUNDOFF→CC120, RESET→CC121, NOTESOFF→CC123, BANK_LSB(70)→CC32,
  SOSTENUTO(76)→CC66, KEYPRES(71)→poly aftertouch (key in HIWORD). RPN
  (5/7/8), reverb/chorus sends, and per-note controllers stay skipped — the
  engine maps none of those CCs.
- 2026-09-11 fix(v3): BASS shim font surface: BASS_MIDI_FontInit keeps one
  slot per path (handles never reused, FontFree marks freed),
  BASS_MIDI_StreamSetFonts parses the real count-flag formats (0x1000000 =
  24-byte FONTEX — verified in Bass.Net's wrapper IL and the genuine
  BASSMIDI copy helper disassembly — else 12-byte FONT), and streams take
  the list's first valid font (BASSMIDI's earlier-entry-wins priority;
  single-font fallback = last FontInit). A post-create StreamSetFonts that
  changes the priority font rebuilds the untouched session.
- 2026-09-11 feat(v3): BASS shim honors BASS_ATTRIB_MIDI_VOICES
  (Kiva's RenderVoices): setting it at frame 0 rebuilds the offline session
  with that pool size; GetAttribute round-trips it and reports
  MIDI_VOICES_ACTIVE via session telemetry.
- 2026-09-11 perf(v3): BASSMIDI offline sessions unthrottled —
  render_threads = 0 (engine auto: hardware concurrency, cap 16), backend
  AUTO (was 1 SCALAR thread), pool 4096 (was 2048), limiter OFF (real
  BASSMIDI emits raw float and Kiva applies its own LoudMax; double
  limiting pumped). This is the shim's "struggles rendering when it can
  clearly give more" fix.
- 2026-09-11 fix(v3): BASS shim StreamEvents — same-frame events keep
  submission order (stable sort; the packed-message tiebreaker reordered
  RPN/data sequences), and mode's low 16 bits now act as the 1-based
  channel override on all three event paths.
- 2026-09-11 fix(v3): BASS ChannelGetData (nullptr, 0) answers the
  available-bytes query (bytes up to the 2 s tail) instead of erroring.
- 2026-09-11 test(v3): bass_chain_probe extended — two font slots, FONT +
  FONTEX SetFonts formats, bad-handle rejection, MIDI_VOICES rebuild
  round-trip, SOUNDOFF/RESET/KEYPRES struct acceptance, available query;
  probe pulls now pass the real 0x40000000 flag.

- 2026-09-09 fix(v3): VEH crash reporter scoped to faults inside our own
  module. Under Kiva + OmniMIDI KDMAPI, OmniMIDI's engine AVs inside
  USER32 wvsprintfA (its own debug formatting) and our first-chance VEH
  walked that foreign stack — destabilizing DebugView (which itself AVs on
  debug streams emitted in that state) and masking the real fault.
- 2026-09-09 fix(v3): BASS shim gained a file diagnostic channel —
  %TEMP%\svms_bass.log (FontInit/StreamCreate/StreamEvents/GetData/ENDED),
  independent of OutputDebugString so it survives the DebugView crash.
- 2026-09-09 fix(v3): BASS shim exports the BASS_FX surface (BASS_FXReset/
  Free/SetParameters/GetParameters/SetPriority/Version) as no-ops. OmniMIDI's
  KDMAPI loader binds its BuiltInEngine's BASS_FX imports to whatever module
  is loaded as bassmidi.dll (its own BASSMIDI build merges BASS_FX); a shim
  without those entry points killed KDMAPI process-wide whenever our
  bassmidi.dll was present alongside it.
- 2026-09-09 fix(v3): BASSMIDI event modes corrected against the real ABI
  (Bass.Net reflection): STRUCT=0, RAW=0x10000, SYNC=0x1000000,
  NORSTATUS=0x2000000, CANCEL=0x4000000, TIME=0x8000000 (the shim had
  SYNC/TIME/CANCEL values shifted). TIME positions by the stream BYTE
  position (RAW block header / BASS_MIDI_EVENT.pos); position-less events
  (RAW without TIME — Kiva's SendEventRaw plain 3-byte messages) anchor at
  the pull cursor. Probe verifies all three realtime shapes + drain.
- 2026-09-09 fix(v3): BASSMIDI flagless StreamEvents = BASS_MIDI_EVENTS_SYNC
  — events apply at the CURRENT pull cursor instead of tick 0. Kiva's
  realtime generator drains up to the event time and then sends flagless
  RAW blocks (RAW|NORSTATUS); anchoring them at the pull position makes the
  shim match the realtime contract (was: every note landed at frame 0 ->
  completely silent output). Probe: send-then-pull sounds, pull-then-send
  leaves the earlier window silent.
- 2026-09-09 fix(v3): BASSMIDI decode streams are FINITE like real BASS —
  GetData past the last event + 2 s tail returns -1/BASS_ERROR_ENDED (45),
  crossing pulls return partial data. Without termination, a broken caller
  length (Kiva Modded's generator wraps its ring count negative -> a
  ~4 GB DWORD length) rendered silence forever past the caller's pinned
  buffer: the Kiva AV in bass.dll. Verified by drain probe: full, full,
  partial, -1/45.
- 2026-09-09 fix(v3): BASS prerender pulls actually render — GetData pumps
  through max_block_frames-bounded chunks (NativeRenderOffline rejects
  frameCount > max_block_frames; the shim asked for unbounded pulls), the
  session requests 64k-frame blocks, FontInit accepts UTF-16 paths with or
  without the BASS_UNICODE flag (BASS.NET marshaling), and FontInit /
  StreamCreate failures LOG their path and native result code.
- 2026-09-09 fix(v3): BASSMIDI StreamCreate first parameter is the MIDI
  channel count, not output channels — Kiva's 16-channel request now passes
  and streams always render the stereo pair (was rejected with a mislabeled
  BUFLOST error code).
- 2026-09-09 fix(v3): BASSMIDI shim surfaces Kiva — added BASS_MIDI_FontLoad,
  BASS_ChannelFlags and BASS_GetVersion exports (host + forwarder), and
  corrected BASS_MIDI_StreamEvents to the real 4-arg bassmidi ABI
  (handle, mode, events, length) with TIME/tick/RAW/CANCEL mode parsing;
  render pulls now report BASS_ERROR_ENDED (45) past the event tail so
  prerender pumps terminate.
