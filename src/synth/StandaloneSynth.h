#ifndef SVMS_STANDALONE_SYNTH_H
#define SVMS_STANDALONE_SYNTH_H

#include <deque>
#include "synth/Tuning.h"
#include "synth/ChannelCache.h"
#include "synth/Config.h"
#include "synth/PresetResolve.h"
#include "render/HalfRateBus.h"
#include "render/SampleBandwidth.h"
#include "render/Envelope.h"
#include "events/EventCompile.h"
#include "render/HilbertPair.h"
#include "render/Limiter.h"
#include "events/NoteMerge.h"
#include "events/NoteOnCollapse.h"
#include "render/Reverb.h"
#include "synth/ThreadAffinity.h"
#include "render/PostFilter.h"
#include "render/RenderScalar.h"
#include "render/ReleaseBus.h"
#include "synth/SoundFont.h"
#include "synth/Oversample.h"
#include "synth/VoiceManager.h"
#include "render/VoiceFilter.h"

#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
#include "render/GpuSynth.h"
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace svms {

// Same-key note-on collapse window, matching the realtime driver's fixed
// 20 ms (Driver.cpp kNoteOnCollapseWindowMs) — expressed in AUDIO FRAMES
// here (the driver uses QPC ticks; the gate is domain-agnostic).
constexpr uint32_t kNoteOnCollapseWindowMs = 20u;

struct StandaloneSynthConfig {
    std::wstring soundfont;
    // Priority-ordered stack (first answers first). Empty = just `soundfont`.
    std::vector<std::wstring> soundfonts;
    uint32_t sampleRate = 44100;
    uint32_t maxVoices = 4096;
    uint32_t renderThreads = 1;
    uint32_t maxBlockFrames = 2048;
    // Experimental: the whole voice engine runs at half the output rate (voice
    // setup, envelopes, events on a 2-frame grid) and the mixed block is
    // upsampled 2x before reverb and limiter. Roughly halves the render cost;
    // content above ~0.4 of the engine Nyquist is lost, and the output lags by
    // 32 frames.  Env SVMS_UNIFORM_HALF_RATE=1 sets it for offline tools.
    bool uniformHalfRate = false;
    // Experimental release bus (ReleaseBus.h): eligible short notes ring out as convolutions instead of voices.
    // Offline only for now. Env SVMS_RELEASE_BUS=1 sets it for offline tools.
    bool releaseBus = false;
    uint32_t releaseBusMb = 512;   // kernel memory budget
    float masterVolume = 1.0f;
    bool limiterEnabled = true;
    LimiterAlgorithm limiterAlgorithm = LimiterAlgorithm::Classic;
    float limiterThreshold = 0.95f;
    float limiterLookaheadMs = 3.0f;
    float limiterAttackMs = 0.5f;
    float limiterReleaseMs = 100.0f;
    uint32_t phaseRotationMode = 0u;
    uint32_t tuningEdo = 12u;
    uint32_t oversample = 1u;  // sample-store oversampling factor (experimental)
    RenderBackend backend = RenderBackend::AVX512; // sentinel: automatic
    // When set, the realtime driver's voice/render tuning is applied from this
    // config (steal policy, per-key cap, ghost budget, decimation, retire
    // floor, fast chunk math, affinity) so an offline render behaves like the
    // live synth under the same settings. Null = engine defaults (BASS shim,
    // Linux daemon, tests).
    const EngineConfig* tuning = nullptr;
    // BASSMIDI voice admission without the rest of the tuning: a full pool skips a
    // note that is not louder than its quietest voice (VoiceManager::SetStealReject).
    bool stealReject = false;
    bool stealAgeless = false;  // with stealReject: BASSMIDI 2018 ranking, no age term
    // Both output channels carry the average of left and right (before the limiter).
    bool monoDownmix = false;
    // GPU render stage (render/GpuStage.h): 0 off, otherwise a gpu::Api value.
    // A stage that cannot start leaves the render on the CPU.
    uint32_t gpuApi = 0u;
    uint32_t gpuDeviceIndex = 0u;
};

// Platform-neutral owner for the existing V3 synthesis core. Event ordering
// and absolute-frame placement remain the responsibility of the caller.
class StandaloneSynth {
public:
    bool Initialize(const StandaloneSynthConfig& config, std::string& error) {
        tuningEdo_ = config.tuningEdo;
        rate_ = config.sampleRate;
        uniformHalf_ = config.uniformHalfRate ||
                       std::getenv("SVMS_UNIFORM_HALF_RATE") != nullptr;
        engineRate_ = uniformHalf_ ? (rate_ + 1u) / 2u : rate_;
        maxVoices_ = config.maxVoices;
        master_ = config.masterVolume;
        rotationMode_ = config.phaseRotationMode;
        oversample_ = config.oversample;
        {
            std::vector<std::wstring> paths = config.soundfonts;
            if (paths.empty()) paths.push_back(config.soundfont);
            if (!LoadBanks(paths, error)) return false;
        }
        if (const EngineConfig* t = config.tuning) ApplyTuning(*t);
        // Env override for offline experiments (percent of the voice limit
        // per block, 0 = off), like the other experimental switches.
        if (const char* bulk = std::getenv("SVMS_BULK_ADMISSION"))
            renderer_.SetBulkAdmissionPercent(
                static_cast<uint32_t>(std::atoi(bulk)));
        monoDownmix_ = config.monoDownmix || (config.tuning && config.tuning->monoDownmix);
        if (config.stealReject) {
            voices_.SetStealReject(true);
            rejectPairing_ = true;
            keyOutcome_.assign(size_t(kChannelCount) * kNoteCount, std::deque<uint8_t>());
        }
        if (config.stealAgeless) voices_.SetStealAgeless(true);
        if (!voices_.Initialize(maxVoices_, engineRate_)) {
            error = "cannot allocate voice storage";
            return false;
        }
        if (!renderer_.ReserveVoiceCapacity(maxVoices_)) {
            error = "cannot allocate renderer scratch";
            return false;
        }
        uint32_t renderThreads = config.renderThreads;
        if (renderThreads == 0u) {
            renderThreads = (std::max)(1u, std::thread::hardware_concurrency());
            renderThreads = (std::min)(16u, renderThreads);
        }
        if (!renderer_.ConfigureRenderThreads(renderThreads,
                                              (config.uniformHalfRate ? (config.maxBlockFrames + 1u) / 2u : config.maxBlockFrames))) {
            error = "cannot initialize render workers";
            return false;
        }
        if (!renderer_.PrepareWholeVoice(voices_)) {
            error = "cannot reserve whole-voice renderer storage";
            return false;
        }
        channels_.Reset();
        channels_.SetMasterVolume(master_);
        cfg_ = {master_, 1.0f, 0.0f, 0, false, false, false, false, false,
                false, InterpolationMode::Linear, FilterType::None,
                PanLaw::ConstantPower, true};
        if (const EngineConfig* t = config.tuning) {
            // Same snapshot the driver builds, so velocity mapping, mono
            // output, pan law and interpolation match the live synth.
            cfg_ = {master_, t->velocityCurve, t->velocityFloor,
                    t->velocityIgnoreBelow, t->ignoreVelocity, t->monoOutput,
                    t->enableReverb, t->enableChorus, t->enableFilter,
                    t->enableModulators, t->interpolation, t->filterType,
                    t->panLaw, t->correctnessMode};
            reverbOn_ = t->enableReverb;
            if (reverbOn_) {
                reverb_.reset(new ReverbState());
                reverb_->Configure(rate_, *t);
            }
        }
        channels_.RebuildCache(cfg_, static_cast<float>(engineRate_));
        for (uint32_t v = 0; v < 128u; ++v) {
            if (!config.tuning) {
                velGain_[v] = float(v) * float(v) / (127.0f * 127.0f);
                continue;
            }
            // The driver's historical quantisation: map, round to a 0..127
            // index, square (its LUT is idx^2/127^2).
            const float mapped = channels_.ComputeVelocity(uint8_t(v), cfg_);
            if (mapped <= 0.0f) { velGain_[v] = 0.0f; continue; }
            uint32_t idx = static_cast<uint32_t>(mapped * 127.0f + 0.5f);
            idx = (std::max)(1u, (std::min)(127u, idx));
            velGain_[v] = float(idx) * float(idx) / (127.0f * 127.0f);
        }
        RebuildPrepared();
        for (uint8_t channel = 0; channel < kChannelCount; ++channel)
            RefreshPreset(channel);
#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
        if (config.backend == RenderBackend::GPU && !uniformHalf_) {
            std::string gpuError;
            if (!gpuSynth_.Initialize(sampleData_.data(),
                                      sampleFrames_,
                                      config.maxVoices, config.maxBlockFrames,
                                      gpuError)) {
                error = "GPU init failed: " + gpuError;
                return false;
            }
            gpuEnabled_ = true;
        } else
#endif
        {
            if (config.backend != RenderBackend::AVX512 &&
                !renderer_.SetRenderBackend(config.backend)) {
                error = "requested render backend is unsupported on this CPU";
                return false;
            }
            gpuEnabled_ = false;
        }
#if defined(SVMS_GPU_STAGE)
        if (config.gpuApi != 0u && !uniformHalf_) {
            std::string gpuError;
            gpuStage_ = GpuStage::Create(static_cast<gpu::Api>(config.gpuApi),
                                         config.gpuDeviceIndex, gpuError);
            if (gpuStage_) {
                gpuStage_->SetSampleRate(rate_);
                // Offline has no deadline: a slow block is waited for.
                gpuStage_->SetDeadlinePercent(1000000u);
                gpuStage_->SetMaxFramesHint(config.maxBlockFrames);
                renderer_.SetGpuStage(gpuStage_.get());
            }
        }
#endif
        for (float& ratio : bendRatio_) ratio = 1.0f;
        sysexMasterVolume_ = 1.0f;
        sysexMasterFineTune_ = 0.0f;
        sysexMasterTranspose_ = 0.0f;
        postHighPass_.Initialize(rate_);
        postHighPass_.SetPassThrough(g_keepDcOffset || std::getenv("SVMS_NO_DCBLOCK") != nullptr);
        EngineConfig limiterConfig{};
        limiterConfig.limiterEnabled = config.limiterEnabled;
        limiterConfig.limiterAlgorithm = config.limiterAlgorithm;
        limiterConfig.limiterThreshold = config.limiterThreshold;
        limiterConfig.limiterLookaheadMs = config.limiterLookaheadMs;
        limiterConfig.limiterAttackMs = config.limiterAttackMs;
        limiterConfig.limiterReleaseMs = config.limiterReleaseMs;
        limiter_.Configure(rate_, limiterConfig);
        if (!voices_.SetPhaseRotationMode(config.phaseRotationMode)) {
            error = "cannot allocate phase-rotation voice state";
            return false;
        }
        // Production event dispatch: RenderBlock walks its event array and
        // calls this at each event's exact frame, so batched event blocks
        // take the whole-voice/dense fast paths instead of per-event span
        // rendering. The whole-voice pre-pass also routes bend ops through
        // VoiceManager::ApplyChannelBendRatio's hook — see
        // RefreshChannelPitch.
        renderer_.SetEventDispatcher(&DispatchRenderEventStatic, this);
        renderer_.SetEventBatchDispatcher(&DispatchRenderEventBatchStatic, this);
        // Batched steal-victim selection inside LaunchVoiceGroup: provably
        // identical victims to the sequential per-layer pops (no candidate
        // insertion can occur inside one launch transaction), at a fraction
        // of the cost under pool pressure.
        voices_.SetStealBatchingEnabled(true);
        // Same-key note-on coalescing, mirroring the realtime driver
        // (NoteOnCollapse.h): the gate runs in the audio-FRAME domain
        // with the same fixed 20 ms window and the same config threshold,
        // so prerender reproduces realtime's event semantics exactly
        // (including the velocity stacking of collapsed hits). Default
        // threshold 1 = disabled, every note-on spawns.
        static const EngineConfig collapseConfig = EngineConfig::Load();
        noteOnCollapse_.SetWindowTicks(
            static_cast<uint64_t>(engineRate_) *
            kNoteOnCollapseWindowMs / 1000u);
        noteOnCollapse_.SetThreshold(collapseConfig.noteOnCollapseThreshold);
        noteOnCollapse_.SetReExciteSpacing(
            static_cast<uint64_t>(engineRate_) * kReExciteAttackMs /
            1000u);
        reExcite_ = collapseConfig.noteOnReexcite &&
                    collapseConfig.noteOnCollapseThreshold > 1u;
        renderer_.SetReExciteMode(reExcite_);
        // Exact note-on merging (NoteMerge.h): the same opt-in the
        // realtime driver reads. Offline renders have the whole event block
        // up front, so the pass runs on each block just before RenderBlock.
        // An exclusive-class SoundFont limits it to adjacent note-ons.
        noteMerge_ = collapseConfig.noteMerge;
        nestedNoteOff_ = collapseConfig.nestedNoteOff;
        if (config.stealAgeless) {
            // BASSMIDI 2018 launches every note-on as its own voice: no merging
            // or coalescing from the user's performance config.
            releaseAll_ = std::getenv("SVMS_BASS_NOTEOFF_ALL") != nullptr;  // experiment only: it removes the buzz without BASSMIDI's tick-quantised release
            holdOnset_ = 12u; holdGain_ = 0.8f;  // oracle: level and onset of the real 2018 voices
            if (const char* v = std::getenv("SVMS_HOLD_ONSET")) holdOnset_ = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
            if (const char* v = std::getenv("SVMS_HOLD_GAIN")) holdGain_ = static_cast<float>(std::atof(v));
            noteMerge_ = false;
            noteOnCollapse_.SetThreshold(1u);
            reExcite_ = false;
            renderer_.SetReExciteMode(false);
        }
        UpdateNoteMergeHoist();
        busOn_ = (config.releaseBus || std::getenv("SVMS_RELEASE_BUS") != nullptr) &&
                 !uniformHalf_ && !gpuEnabled_ && oversample_ <= 1u;
        busBudget_ = size_t(config.releaseBusMb) << 20;
        busBlock_ = config.maxBlockFrames;
        if (busOn_) StartBus();
        return true;
    }

    // Replaces the SoundFont stack of a running session (call between
    // renders). All sounding voices are cut; per-channel assignments stay.
    // On failure the old stack keeps playing.
    // `filters` (optional, one per path) restricts an entry to one bank
    // and/or program: {bank, program}, -1 = any; bank 128 = percussion.
    bool SetSoundFonts(const std::vector<std::wstring>& paths,
                       std::string& error,
                       const std::vector<std::pair<int32_t, int32_t>>* filters =
                           nullptr) {
        if (gpuEnabled_) {
            error = "the GPU backend cannot change SoundFonts";
            return false;
        }
        if (busOn_) busKernels_.Stop();   // its kernels name the old stack's regions
        if (!LoadBanks(paths, error, filters)) {
            if (busOn_) StartBus();
            return false;
        }
        for (uint8_t channel = 0; channel < kChannelCount; ++channel)
            voices_.SilenceChannelImmediate(channel);
        RebuildPrepared();
        UpdateNoteMergeHoist();
        for (uint8_t channel = 0; channel < kChannelCount; ++channel)
            RefreshPreset(channel);
        if (busOn_) StartBus();
        return true;
    }

    // font 0 = automatic, N = stack entry N (1-based) answers the channel first.
    bool SetChannelSoundFont(uint8_t channel, uint32_t font) {
        ++levelEpoch_;
        if (channel >= kChannelCount || font > banks_.size()) return false;
        channelFont_[channel] = uint8_t(font);
        RefreshPreset(channel);
        return true;
    }

    uint32_t SoundFontCount() const { return uint32_t(banks_.size()); }

    // Release bus: what the voice path reads for region `localRegion` of font `font` on `note`, held, at velocity
    // gain 1, no bend and unity channel mix (ReleaseKernels.h). Read-only, so the kernel builder may call it from
    // its own thread while the SoundFont stack stays loaded. False if the region is not playable.
    bool ReleaseKernelSource(uint32_t font, uint32_t localRegion, uint8_t note,
                             bus::KernelSource& out) const {
        if (font >= banks_.size() || note >= kNoteCount) return false;
        const Bank& bank = banks_[font];
        if (localRegion >= bank.data->regionCount) return false;
        const uint32_t index = bank.regionBase + localRegion;
        if (index >= prepared_.size() || !prepared_[index].valid) return false;
        const SFSampleRegion& region = bank.data->regions[localRegion];
        const PreparedRegion& p = prepared_[index];
        out = bus::KernelSource{};
        out.samples = sampleData_.data();
        out.sampleFrames = sampleFrames_;
        out.sampleStart = bank.sampleBase + uint32_t(region.startOffset);
        out.sampleEnd = bank.sampleBase + uint32_t(region.endOffset);
        out.loopStart = bank.sampleBase + uint32_t(region.loopStartOffset);
        out.loopEnd = bank.sampleBase + uint32_t(region.loopEndOffset);
        out.loopMode = region.loopMode;
        out.phaseStep = p.baseStep[note];
        out.gain = p.attenuation * holdGain_;
        out.panL = p.panL;
        out.panR = p.panR;
        out.delay = p.delay;
        out.hold = p.hold;
        out.attack = holdOnset_ ? holdOnset_ : p.attack;
        out.decay = p.decay;
        out.decaySlope = p.decaySlope;
        out.sustain = p.sustain;
        out.releaseDecay = p.releaseDecay;
        out.releaseFrames = p.release;
        out.filter = p.filter;
        return true;
    }
    float VelocityGain(uint8_t velocity) const { return velGain_[velocity & 127u]; }
    // The channel's current mix gains (volume, expression, pan, master) that multiply every voice on it.
    void ChannelMixScale(uint8_t channel, float& left, float& right) const {
        const ChannelParamsSnapshot& cp = channels_.GetParams()[channel & 15u];
        left = cp.mixScaleLeft;
        right = cp.mixScaleRight;
    }
    bool ReleaseBusOn() const { return busOn_; }
    uint64_t ReleaseBusNotes() const { return busNotes_; }
    uint64_t ReleaseBusHanded() const { return busHanded_; }
    uint64_t ReleaseBusMoves() const { return bus_.Moves(); }
    double ReleaseBusConvolutionsPerBlock() const {
        return bus_.Blocks() ? double(bus_.Convolved()) / double(bus_.Blocks()) : 0.0;
    }
    void SetReleaseBusSharing(bool on) { busShare_ = on; bus_.SetSharing(on); }
    // A host that knows what is coming (the offline renderer) calls EnableBusLookahead before rendering, then gives
    // the events up to the returned frames past each render call's end, sorted, before the call (SetBusLookahead;
    // events an earlier call gave may be left out), and clears it after. The bus pairs a note-on with a note-off
    // that far ahead (the engine's pairing, run ahead: FeedAhead), so an eligible note shorter than its direct span
    // never takes a voice, and its kernels keep level 0: such a note costs a correction of its held frames, not its
    // first 8 blocks.
    // Handoffs stop (a late impulse cannot join level 0). Without it, pairs stay within a call. 0: no bus.
    uint32_t EnableBusLookahead() {
        if (!busOn_) return 0u;
        if (!busAheadOn_) {
            busAheadOn_ = true;
            StartBus();   // the delayed layout, kernels with level 0
        }
        return bus_.Direct();
    }
    void SetBusLookahead(const RealtimeEvent* events, size_t count) {
        busAhead_ = events;
        busAheadCount_ = count;
    }
    uint32_t BusLookaheadFrames() const { return busOn_ && busAheadOn_ ? bus_.Direct() : 0u; }
    // With the lookahead the bus renders behind its events (ReleaseBus::Configure), and the voices go through a
    // delay line to meet it: output comes this many frames late. The host drops that many frames first and renders
    // as many more at the end. SVMS_BUS_DELAY=0: the 1, 4, 16 block layout, no delay.
    uint32_t OutputDelay() const { return busOn_ ? busDelay_ : 0u; }
    size_t ReleaseBusHistories() const { return bus_.Histories(); }
    size_t ReleaseBusKernelBytes() const { return busKernels_.Bytes(); }
    double BusMsPerSecond(uint64_t ns) const {
        return bus_.Blocks() ? ns / 1e6 / (double(bus_.Blocks()) * busBlock_ / rate_) : 0.0;
    }

    ~StandaloneSynth() {
#if defined(SVMS_GPU_STAGE)
        renderer_.SetGpuStage(nullptr);
#endif
        if (busOn_)
            std::fprintf(stderr, "[SVMS] release bus: %llu same-block and %llu handed-off notes of %llu, %.1f MB of "
                         "kernels, %u regions refused, "
                         "%.0f convolutions per block, %llu mix moves; ms per audio second: %.0f convolving, %.0f mixing, "
                         "%.0f planning, %.0f building kernels\n",
                         (unsigned long long)busNotes_, (unsigned long long)busHanded_,
                         (unsigned long long)noteCalls_,
                         busKernels_.Bytes() / 1e6, busKernels_.Refused(),
                         bus_.Blocks() ? double(bus_.Convolved()) / double(bus_.Blocks()) : 0.0, (unsigned long long)bus_.Moves(),
                         BusMsPerSecond(bus_.BusNs()), BusMsPerSecond(busRenderNs_ - bus_.BusNs()),
                         BusMsPerSecond(busPlanNs_), BusMsPerSecond(busKernels_.BuildNs()));
        // Opt-in end-of-session diagnostic: every plan refusal is a skipped
        // stretch (silence), so a real file should report zero.
        if (std::getenv("SVMS_REPORT_REFUSALS") != nullptr) {
            std::fprintf(stderr,
                "[SVMS] whole-voice blocks=%llu refusals: rotation=%llu "
                "event_type=%llu no_dispatcher=%llu reservation=%llu "
                "scalar_vibrato=%llu forced=%llu seg_calls=%llu "
                "seg_fallback=%llu ghost_overflow=%llu\n",
                (unsigned long long)renderer_.GetWholeVoiceBlocksForTest(),
                (unsigned long long)renderer_.GetRefusalCount(RenderScalar::kRefuseRotation),
                (unsigned long long)renderer_.GetRefusalCount(RenderScalar::kRefuseEventType),
                (unsigned long long)renderer_.GetRefusalCount(RenderScalar::kRefuseNoDispatcher),
                (unsigned long long)renderer_.GetRefusalCount(RenderScalar::kRefuseReservation),
                (unsigned long long)renderer_.GetRefusalCount(RenderScalar::kRefuseScalarVibrato),
                (unsigned long long)renderer_.GetRefusalCount(RenderScalar::kRefuseForced),
                (unsigned long long)renderer_.GetWvSegCalls(),
                (unsigned long long)renderer_.GetWvSegFallback(),
                (unsigned long long)renderer_.GetGhostOverflowCount());
        }
        if (std::getenv("SVMS_REPORT_REFUSALS") != nullptr) {
            // Cycles (TSC) per block phase over the whole session.
            std::fprintf(stderr,
                "[SVMS] whole-voice cycles: plan=%llu jobs=%llu post=%llu\n",
                (unsigned long long)renderer_.GetWvPlanCycles(),
                (unsigned long long)renderer_.GetWvJobCycles(),
                (unsigned long long)renderer_.GetWvPostCycles());
        }
        if (std::getenv("SVMS_REPORT_REFUSALS") != nullptr &&
            renderer_.GetHalfRateRows() != 0u)
            std::fprintf(stderr, "[SVMS] half-rate rows=%llu frames=%llu\n",
                (unsigned long long)renderer_.GetHalfRateRows(),
                (unsigned long long)renderer_.GetHalfRateFrames());
        if (gpuStage_ && std::getenv("SVMS_REPORT_REFUSALS") != nullptr) {
            const GpuStageStats& g = gpuStage_->Stats();
            const unsigned long long onGpu = g.rows + g.envRows;
            const unsigned long long total = onGpu + g.cpuRows + g.declinedRows;
            std::fprintf(stderr,
                "[SVMS] gpu %s: considered=%llu gpu_blocks=%llu rows gpu=%llu (sustained %llu, "
                "envelope %llu, timeline %llu) cpu-in-gpu-block=%llu declined-block=%llu "
                "gpu-share=%.1f%% declined(no-store=%llu buses=%llu rotation=%llu "
                "decim/vibrato=%llu min-voices=%llu prepare=%llu few-eligible=%llu "
                "submit-failed=%llu) late=%llu failed=%llu\n",
                gpuStage_->Name(), (unsigned long long)g.considered,
                (unsigned long long)g.blocks, onGpu, (unsigned long long)g.rows,
                (unsigned long long)g.envRows, (unsigned long long)g.timelineRows,
                (unsigned long long)g.cpuRows, (unsigned long long)g.declinedRows,
                total ? 100.0 * (double)onGpu / (double)total : 0.0,
                (unsigned long long)g.declined[0], (unsigned long long)g.declined[1],
                (unsigned long long)g.declined[2], (unsigned long long)g.declined[3],
                (unsigned long long)g.declined[4], (unsigned long long)g.declined[5],
                (unsigned long long)g.declined[6], (unsigned long long)g.declined[7],
                (unsigned long long)g.late, (unsigned long long)g.failed);
            std::fprintf(stderr,
                "[SVMS] gpu ms: classify=%.0f pack=%.0f upload=%.0f sort=%.0f submit=%.0f "
                "wait=%.0f scatter=%.0f\n",
                g.classifyMicros / 1000.0, g.packMicros / 1000.0, g.uploadMicros / 1000.0,
                g.sortMicros / 1000.0, g.submitMicros / 1000.0, g.waitMicros / 1000.0,
                g.scatterMicros / 1000.0);
        }
        for (Bank& bank : banks_) sf2_free(bank.data.get());
    }

    StandaloneSynth() = default;
    StandaloneSynth(const StandaloneSynth&) = delete;
    StandaloneSynth& operator=(const StandaloneSynth&) = delete;

    void Dispatch(uint32_t message, uint64_t frame) {
        busFrame_ = frame;
        voices_.SetCurrentFrame(EngineFrame(frame));
        if (IsInternalEngineMessage(message)) {
            DispatchInternal(message, frame);
            return;
        }
        const uint8_t status = uint8_t(message);
        const uint8_t channel = status & 15u;
        const uint8_t first = uint8_t(message >> 8u);
        const uint8_t second = uint8_t(message >> 16u);
        switch (status & 0xf0u) {
        case 0x80: NoteOff(channel, first); break;
        case 0x90:
            if (second) NoteOn(channel, first, second);
            else NoteOff(channel, first);
            break;
        case 0xb0: Control(channel, first, second); break;
        case 0xc0: Program(channel, first); break;
        case 0xe0: Bend(channel, first, second); break;
        default: break;
        }
    }

    void Render(float* left, float* right, uint32_t frameCount,
                uint64_t absoluteFrame) {
        if (uniformHalf_) {
            RenderWithEventsHalf(nullptr, 0u, left, right, frameCount,
                                 absoluteFrame);
            return;
        }
        std::fill(left, left + frameCount, 0.0f);
        std::fill(right, right + frameCount, 0.0f);
#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
        if (gpuEnabled_ && !GpuFallbackNeeded()) {
            // Same block-boundary housekeeping the CPU RenderBlock performs.
            voices_.SetStealKeyBackend(renderer_.GetRenderBackend());
            voices_.ApplyRuntimeVoiceLimit(absoluteFrame);
            // Steal tails live in a separate reserve and keep running on the
            // CPU per frame; the GPU then accumulates the primary voices on
            // top, matching the CPU addition order (tails before voices).
            for (uint32_t f = 0; f < frameCount; ++f) {
                float* oL = left + f, * oR = right + f;
                const uint32_t tailCount = voices_.GetStealTailCount();
                const uint32_t* tailHandles = voices_.GetStealTailList();
                for (uint32_t pos = tailCount; pos > 0u; --pos) {
                    const uint32_t slot = tailHandles[pos - 1u];
                    RenderStealTailSample(
                        voices_.v, slot, sampleData_.data(),
                        hilbertData_.empty() ? nullptr : hilbertData_.data(),
                        sampleFrames_,
                                          oL, oR);
                    voices_.RefreshStealTail(
                        static_cast<VoiceHandle>(slot));
                }
            }
            voices_.SetCurrentFrame(absoluteFrame);
            gpuSynth_.RenderBlock(voices_, left, right, frameCount,
                                  absoluteFrame);
            ApplyReverb(left, right, frameCount);
            ApplyMono(left, right, frameCount);
            limiter_.ProcessPlanar(left, right, frameCount, postHighPass_);
            return;
        }
#endif
        {
            if (busOn_) BusRetire(absoluteFrame, frameCount);
            renderer_.RenderBlock(
                voices_, channels_, sampleData_.data(),
                hilbertData_.empty() ? nullptr : hilbertData_.data(),
                                  sampleFrames_, left, right,
                                  frameCount, cfg_, nullptr, 0, true,
                                  absoluteFrame);
            if (busOn_) {
                RenderBus(left, right, frameCount, absoluteFrame);
            }
            if (!rawMix_) PostProcess(left, right, frameCount);
        }
    }

    // Same events, one same-frame run at a time, so a run of note-ons can use
    // the lazy same-frame launch the realtime driver uses (a no-op unless
    // voices.drop_zero_life_victims is on). Whatever is pending lands before
    // any other event is handled.
    static void DispatchRenderEventBatchStatic(const RenderEvent* events,
                                               uint32_t count, uint32_t frame,
                                               void* userData) {
        StandaloneSynth* self = static_cast<StandaloneSynth*>(userData);
        if (!self) return;
        bool lazy = false;
        for (uint32_t i = 0u; i < count; ++i) {
            if (events[i].type == RenderEventType::NoteOn) {
                if (!lazy) lazy = self->voices_.BeginLazyLaunchRun();
            } else if (lazy) {
                self->voices_.EndLazyLaunchRun();
                lazy = false;
            }
            DispatchRenderEventStatic(events[i], frame, self);
        }
        if (lazy) self->voices_.EndLazyLaunchRun();
    }

    // Production EventDispatcher: applies one render event to the synth
    // state at the caller's (renderer's) exact frame. Deliberately does NOT
    // touch SetCurrentFrame — the renderer owns the render clock, matching
    // Driver::DispatchRenderEvent. ChannelPressure has no standalone
    // handler (parity with Dispatch()'s switch); it is consumed as a no-op.
    // The event's frame is the render clock's, not the block start plus
    // the cursor: a block split into stretches passes cursors relative to
    // the stretch.
    static void DispatchRenderEventStatic(const RenderEvent& event,
                                          uint32_t /*blockCursor*/,
                                          void* userData) {
        StandaloneSynth* self = static_cast<StandaloneSynth*>(userData);
        if (!self) return;
        const uint64_t frame = self->voices_.GetCurrentFrame();
        self->busFrame_ = frame;
        switch (event.type) {
        case RenderEventType::NoteOn: {
            // Same-key coalescing in the audio-frame domain — the offline
            // mirror of the realtime driver's QPC-domain gate: same 20 ms
            // window, same config threshold, same velocity stacking of
            // collapsed hits. Disabled (every note-on spawns) when the
            // config threshold < 2.
            const uint32_t keyIndex =
                static_cast<uint32_t>(event.channel) * kNoteCount + event.data1;
            uint32_t stack = 0u;
            if (!self->noteOnCollapse_.OnNoteOn(
                    keyIndex, frame,
                    stack)) {
                // Re-excite: the hit is not dropped, it restarts the key's
                // live voices right here at its exact frame. With no live
                // voice it launches normally, so no note is ever lost.
                if (self->reExcite_) {
                    if (!self->noteOnCollapse_.AllowReExcite(
                            keyIndex,
                            frame)) {
                        ++self->coalescedNoteOns_;
                        break;
                    }
                    if (self->voices_.ReExciteKey(event.channel,
                                                  event.data1) != 0u) {
                        ++self->reExcitedNoteOns_;
                        break;
                    }
                }
                if (!self->reExcite_) {
                    ++self->coalescedNoteOns_;
                    break;
                }
            }
            uint8_t velocity = event.data2;
            if (stack > 1u && velocity < 127u) {
                // Collapsed hits are density, not silence: feed the
                // accumulated stack into the spawned voice's velocity
                // (+2 units per doubling), matching the realtime driver.
                uint32_t boost = 0u;
                for (uint32_t s = stack; s >>= 1u;) ++boost;
                boost *= 2u;
                velocity = static_cast<uint8_t>(
                    velocity + boost > 127u ? 127u : velocity + boost);
            }
            if (self->busOn_ && self->BusNoteOn(event.channel, event.data1, velocity, event.count)) break;
            self->NoteOn(event.channel, event.data1, velocity, event.count);
            if (self->busOn_) self->BusTrack(event.channel, event.data1, velocity, event.count);
            break;
        }
        case RenderEventType::NoteOff:
            if (self->busOn_ && self->BusNoteOff(event.channel, event.data1)) break;
            self->NoteOff(event.channel, event.data1);
            break;
        case RenderEventType::StaleNoteOffBatch:
            self->NoteOff(event.channel, event.data1);
            break;
        case RenderEventType::ControlChange:
            if (event.data1 == 120u || event.data1 == 123u)
                self->noteOnCollapse_.ResetChannel(event.channel);
            self->Control(event.channel, event.data1, event.data2);
            break;
        case RenderEventType::ProgramChange:
            self->Program(event.channel, event.data1);
            break;
        case RenderEventType::PitchBend:
            self->Bend(event.channel, event.data1, event.data2);
            break;
        default:
            break;
        }
    }

    // Render one block with sub-block events dispatched at their exact
    // frames through the production RenderBlock machinery (whole-voice
    // renderer, worker pool). Events must be sorted by
    // frameOffset and carry offsets relative to this block; events at
    // frameOffset == frameCount must be held back by the caller and applied
    // via ApplyRenderEvent (outside tested dispatch territory).
    void RenderWithEvents(const RenderEvent* events, uint32_t eventCount,
                          float* left, float* right, uint32_t frameCount,
                          uint64_t absoluteFrame) {
        if (uniformHalf_) {
            RenderWithEventsHalf(events, eventCount, left, right, frameCount,
                                 absoluteFrame);
            return;
        }
        std::fill(left, left + frameCount, 0.0f);
        std::fill(right, right + frameCount, 0.0f);
        if (noteMerge_ && eventCount > 1u) {
            mergeScratch_.assign(events, events + eventCount);
            uint64_t folded = 0u;
            eventCount = noteMerger_.MergeEvents(
                mergeScratch_.data(), eventCount, noteMergeHoist_, folded);
            mergedNoteOns_ += folded;
            events = mergeScratch_.data();
        }
        if (busOn_) {
            const auto busT0 = std::chrono::steady_clock::now();
            BusPlan(events, eventCount, absoluteFrame);
            busPlanNs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - busT0).count());
            BusRetire(absoluteFrame, frameCount);
        }
#if defined(_MSC_VER)
        const uint64_t renderBegin = __rdtsc();
#endif
        renderer_.RenderBlock(
                voices_, channels_, sampleData_.data(),
                hilbertData_.empty() ? nullptr : hilbertData_.data(),
                              sampleFrames_, left, right, frameCount, cfg_,
                              events, eventCount, false, absoluteFrame);
#if defined(_MSC_VER)
        renderCycles_ += __rdtsc() - renderBegin;
#endif
        if (busOn_) {
            busPlan_.clear();
            RenderBus(left, right, frameCount, absoluteFrame);
        }
        if (!rawMix_) PostProcess(left, right, frameCount);
    }

    // Uniform half-rate: half-rate sample k stands for output frame 2k.  A span
    // of output frames [s, e) renders the half samples with 2k inside it, so
    // consecutive spans tile exactly, and an event at output frame f fires
    // before the first half sample at or after f (at most one frame late).
    // A streaming 2x interpolator rebuilds the output (32 frames late) into a
    // FIFO; reverb and limiter then run at the full rate.
    uint64_t EngineFrame(uint64_t frame) const {
        return uniformHalf_ ? (frame + 1u) >> 1 : frame;
    }

    void RenderWithEventsHalf(const RenderEvent* events, uint32_t eventCount,
                              float* left, float* right, uint32_t frameCount,
                              uint64_t absoluteFrame) {
        constexpr uint32_t kHist = 2u * halfrate::kLead;
        const uint64_t k0 = (absoluteFrame + 1u) >> 1;
        const uint64_t k1 = (absoluteFrame + frameCount + 1u) >> 1;
        const uint32_t half = static_cast<uint32_t>(k1 - k0);
        // Events held over from a span with no half sample, then this span's.
        halfEvents_.assign(halfPending_.begin(), halfPending_.end());
        halfPending_.clear();
        for (uint32_t i = 0; i < eventCount; ++i) {
            RenderEvent e = events[i];
            const uint64_t at =
                ((absoluteFrame + e.frameOffset + 1u) >> 1) - k0;
            if (at >= half) {
                e.frameOffset = 0u;
                halfPending_.push_back(e);
            } else {
                e.frameOffset = static_cast<uint32_t>(at);
                halfEvents_.push_back(e);
            }
        }
        for (int c = 0; c < 2; ++c) {
            if (halfBlock_[c].size() < kHist + half + 8u) {
                std::vector<float> grown(kHist + half + 8u, 0.0f);
                std::copy(halfBlock_[c].begin(), halfBlock_[c].begin() +
                          (std::min)(halfBlock_[c].size(), size_t(kHist)),
                          grown.begin());
                halfBlock_[c].swap(grown);
            }
            halfOut_[c].assign(size_t(half) * 2u, 0.0f);
            halfAcc_[c].assign(half + 1u, 0.0f);
            std::fill(halfBlock_[c].begin() + kHist,
                      halfBlock_[c].begin() + kHist + half, 0.0f);
        }
        if (half != 0u) {
            float* const bodyL = halfBlock_[0].data() + kHist;
            float* const bodyR = halfBlock_[1].data() + kHist;
            const RenderEvent* run = halfEvents_.data();
            uint32_t runCount = static_cast<uint32_t>(halfEvents_.size());
            if (noteMerge_ && runCount > 1u) {
                mergeScratch_.assign(run, run + runCount);
                uint64_t folded = 0u;
                runCount = noteMerger_.MergeEvents(
                    mergeScratch_.data(), runCount, noteMergeHoist_, folded);
                mergedNoteOns_ += folded;
                run = mergeScratch_.data();
            }
            renderer_.RenderBlock(
                voices_, channels_, sampleData_.data(),
                hilbertData_.empty() ? nullptr : hilbertData_.data(),
                sampleFrames_, bodyL, bodyR, half, cfg_, run, runCount, false,
                k0);
            halfrate::UpsampleAdd(halfBlock_[0].data(), halfBlock_[1].data(),
                                  halfOut_[0].data(), halfOut_[1].data(),
                                  half * 2u, halfAcc_[0].data(),
                                  halfAcc_[1].data());
            for (int c = 0; c < 2; ++c) {   // the last 32 samples are history
                halfFifo_[c].insert(halfFifo_[c].end(), halfOut_[c].begin(),
                                    halfOut_[c].end());
                std::copy(halfBlock_[c].begin() + half, halfBlock_[c].begin() +
                          half + kHist, halfBlock_[c].begin());
            }
        } else {
            // No half sample in this span: everything waits for the next one.
            halfPending_.insert(halfPending_.begin(), halfEvents_.begin(),
                                halfEvents_.end());
            halfEvents_.clear();
        }
        for (int c = 0; c < 2; ++c) {
            std::vector<float>& fifo = halfFifo_[c];
            float* out = c == 0 ? left : right;
            const size_t have = fifo.size() - halfFifoHead_;
            const size_t take = (std::min)(have, size_t(frameCount));
            std::copy(fifo.begin() + halfFifoHead_,
                      fifo.begin() + halfFifoHead_ + take, out);
            std::fill(out + take, out + frameCount, 0.0f);
            if (c == 1) {
                halfFifoHead_ += take;
                if (halfFifoHead_ > 65536u) {
                    for (int q = 0; q < 2; ++q)
                        halfFifo_[q].erase(halfFifo_[q].begin(),
                                           halfFifo_[q].begin() + halfFifoHead_);
                    halfFifoHead_ = 0;
                }
            }
        }
        if (!rawMix_) PostProcess(left, right, frameCount);
    }

    // Key shards (offline --shards) render raw; the owner of the summed mix
    // runs reverb + limiter once over it.
    void SetRawMix(bool raw) { rawMix_ = raw; }
    float StealLevel(uint8_t channel, uint8_t note) const {
        return voices_.StealLevelOfKey(channel, note);
    }
    uint32_t VoiceCount(uint8_t channel, uint8_t note) const {
        return voices_.VoiceCountOfKey(channel, note);
    }
    void PostProcess(float* left, float* right, uint32_t frameCount) {
        ApplyReverb(left, right, frameCount);
        ApplyMono(left, right, frameCount);
        limiter_.ProcessPlanar(left, right, frameCount, postHighPass_);
    }

    uint32_t GetRenderPaths() const { return renderer_.GetLastRenderPaths(); }
    uint64_t RenderCycles() const { return renderCycles_; }
    uint64_t CoalescedNoteOns() const { return coalescedNoteOns_; }
    uint64_t ReExcitedNoteOns() const { return reExcitedNoteOns_; }
    // Note-ons folded into a neighbour by exact note merging, and the number
    // of voices the pool would hold had nothing been merged.
    uint64_t MergedNoteOns() const { return mergedNoteOns_; }
    uint64_t EffectiveVoices() { return voices_.CountEffectiveVoices(); }
    uint64_t WvPlanCycles() const { return renderer_.GetWvPlanCycles(); }
    uint64_t WvJobCycles() const { return renderer_.GetWvJobCycles(); }
    uint64_t WvPostCycles() const { return renderer_.GetWvPostCycles(); }
    uint64_t WvSegCalls() const { return renderer_.GetWvSegCalls(); }
    uint64_t WvSegCycles() const { return renderer_.GetWvSegCycles(); }
    uint64_t WvSegKernelOk() const { return renderer_.GetWvSegKernelOk(); }
    uint64_t WvSegFallback() const { return renderer_.GetWvSegFallback(); }
    uint64_t WvSegKernelFrames() const {
        return renderer_.GetWvSegKernelFrames();
    }
    uint64_t WvSegFallbackFrames() const {
        return renderer_.GetWvSegFallbackFrames();
    }

    void SetCursor(uint64_t absoluteFrame) {
        voices_.SetCurrentFrame(EngineFrame(absoluteFrame));
    }

    void ApplyRenderEvent(const RenderEvent& event) {
        DispatchRenderEventStatic(event, 0u, this);
    }

    void ReleaseAll() {
        for (uint8_t channel = 0; channel < kChannelCount; ++channel)
            voices_.ReleaseChannel(channel, 0);
    }

    void ResetAll(uint64_t absoluteFrame) {
        ++levelEpoch_;
        noteOnCollapse_.ResetAll();
        coalescedNoteOns_ = 0;
        reExcitedNoteOns_ = 0;
        voices_.SetCurrentFrame(EngineFrame(absoluteFrame));
        for (uint8_t channel = 0; channel < kChannelCount; ++channel)
            voices_.SilenceChannelImmediate(channel);
        channels_.Reset();
        channels_.SetMasterVolume(master_);
        sysexMasterVolume_ = 1.0f;
        sysexMasterFineTune_ = 0.0f;
        sysexMasterTranspose_ = 0.0f;
        channels_.RebuildCache(cfg_, static_cast<float>(engineRate_));
        for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
            bendRatio_[channel] = 1.0f;
            RefreshPreset(channel);
        }
        if (busOn_) {
            busFrame_ = absoluteFrame;
            if (busDelay_) {   // ahead of the delayed bus's output: its tails stop there
                for (uint8_t channel = 0; channel < kChannelCount; ++channel) bus_.Cut(channel, absoluteFrame);
            } else {
                bus_.Reset(absoluteFrame);
            }
            std::fill(keyHeld_.begin(), keyHeld_.end(), 0u);
            std::fill(busOff_.begin(), busOff_.end(), UINT64_MAX);
            for (BusKey& k : busKeys_) k.offs.clear();   // the lookahead's pairing (open, pairs) is ahead of this
            busLate_.clear();
            busLateOrder_.clear();
            busKill_.clear();
            for (uint8_t channel = 0; channel < kChannelCount; ++channel) BusMix(channel);
        }
    }

    // Live parameter changes. Audio-thread only (call between blocks): the
    // control thread parks the request and the render loop applies it.
    void SetMasterVolume(float volume) {
        ++levelEpoch_;
        master_ = volume;
        channels_.SetMasterVolume(master_ * sysexMasterVolume_);
        channels_.RebuildCache(cfg_, static_cast<float>(engineRate_));
        for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
            voices_.MarkChannelMixStale(channel, channels_.GetParams()[channel]);
            BusMix(channel);
        }
    }

    void ConfigureLimiter(bool enabled, LimiterAlgorithm algorithm,
                          float threshold, float lookaheadMs, float attackMs,
                          float releaseMs) {
        EngineConfig limiterConfig{};
        limiterConfig.limiterEnabled = enabled;
        limiterConfig.limiterAlgorithm = algorithm;
        limiterConfig.limiterThreshold = threshold;
        limiterConfig.limiterLookaheadMs = lookaheadMs;
        limiterConfig.limiterAttackMs = attackMs;
        limiterConfig.limiterReleaseMs = releaseMs;
        limiter_.Configure(rate_, limiterConfig);
    }

    uint32_t Active() const { return voices_.GetActiveCount(); }
    uint32_t Tails() const { return voices_.GetStealTailCount(); }
    void ChannelVoices(uint32_t (&out)[16]) const {
        for (uint8_t c = 0; c < 16; ++c) {
            uint32_t n = 0;
            voices_.ForEachChannelActive(c, [&](VoiceHandle) { ++n; });
            out[c] = n;
        }
    }
    uint32_t Steals() const { return voices_.stealCount_; }
    uint32_t ClassCount(uint32_t renderClass) const {
        return voices_.GetRenderClassCount(static_cast<VoiceRenderClass>(renderClass));
    }
    uint64_t LazyChainSteps() const { return voices_.GetLazyChainSteps(); }
    uint64_t LazyStarts() const { return voices_.GetLazyStarts(); }
    // ── Temporary dispatch-phase profiler (diagnostics only) ────────────
    struct DispatchProfile {
        uint64_t noteOnCalls = 0, noteOffCalls = 0;
        uint64_t noteOnTotal = 0, noteOffTotal = 0;
        uint64_t resolve = 0, alloc = 0, configure = 0;
        uint64_t controlCalls = 0, controlTotal = 0;
        uint64_t controlRebuild = 0, controlMix = 0;
        uint64_t programCalls = 0, bendCalls = 0, bendTotal = 0;
    };
    DispatchProfile dispatchProfile;

    uint32_t Free() const { return maxVoices_ - voices_.GetActiveCount(); }
    uint64_t StealHeapBuildCount() const {
        return voices_.GetStealHeapBuildCount();
    }
    uint64_t NoteCalls() const { return noteCalls_; }
    uint64_t MatchedNotes() const { return notes_; }
    uint64_t PhysicalLaunches() const { return physicalLaunches_; }
    uint64_t RetiredVoices() const { return voices_.retireCount_; }
    uint64_t MissingPresets() const { return missingPresets_; }
    uint64_t MissingRegions() const { return missingRegions_; }
    uint64_t InvalidRegions() const { return invalidRegions_; }
    uint64_t FallbackRegions() const { return fallbackRegions_; }
    const char* Backend() const {
#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
        if (gpuEnabled_) return "gpu (d3d11)";
#endif
        return renderer_.GetRenderBackendName();
    }
    // Test/diagnostic access to the render-path counters.
    // The engine-config switches the offline synth honours (Initialize calls
    // this when the config carries a tuning block).
    void ApplyTuning(const EngineConfig& t) {
        voices_.SetPerKeyVoiceCap(t.perKeyVoiceCap);
        voices_.SetStealReject(t.stealReject);
        g_voiceRetireThreshold.store(t.voiceRetireThreshold,
                                     std::memory_order_relaxed);
        g_threadAffinityMode.store(t.threadAffinityMode,
                                   std::memory_order_relaxed);
        renderer_.SetGhostBudget(t.ghostBudget);
        renderer_.SetDecimationEnabled(t.decimation);
        renderer_.SetHalfRate(t.halfRate, 0.0f);
        renderer_.SetDecimationByLevel(t.decimationByLevel);
        renderer_.SetMutedTailCullBlocks(t.decimationCullBlocks);
        SetFastChunkMath(t.fastChunkMath);
        renderer_.SetLeanVoices(t.leanVoices);
        renderer_.SetBulkAdmissionPercent(t.bulkAdmissionPercent);
    }
    bool LeanVoicesForTest() const { return renderer_.LeanVoices(); }
    void SetRowOpBudgetForTest(uint32_t budget) { renderer_.SetRowOpBudgetForTest(budget); }

    uint64_t WholeVoiceBlocksForTest() const {
        return renderer_.GetWholeVoiceBlocksForTest();
    }

private:
    struct PreparedRegion {
        float baseStep[kNoteCount];
        float bendScale, attenuation, sustain, decaySlope, releaseDecay;
        PreparedVoiceFilter filter{};
        float panL, panR;
        uint32_t delay, hold, attack, decay, release;
        bool valid;
        uint8_t halfBand = 0;  // sample bandwidth code, 0 = unknown
    };

    // Launch-transaction scratch: the region group's configurations are
    // built up front so LaunchVoiceGroup can resolve the batched victim
    // selection once for the whole note-on (production launch path).
    VoiceConfiguration launchSetups_[512];
    // Same-key note-on coalescing (audio-frame domain). See Initialize.
    NoteOnCollapseGate noteOnCollapse_;
    uint64_t coalescedNoteOns_ = 0;
    uint64_t reExcitedNoteOns_ = 0;
    bool reExcite_ = false;
    // Exact note-on merging (see Initialize / RenderWithEvents).
    bool noteMerge_ = false;
    bool nestedNoteOff_ = false;  // a note-off ends the newest open note
    bool rawMix_ = false;
    uint32_t oversample_ = 1u;
    bool noteMergeHoist_ = true;
    NoteMerger noteMerger_;
    std::vector<RenderEvent> mergeScratch_;
    uint64_t mergedNoteOns_ = 0;

    struct RegionCacheEntry {
        uint64_t tag = UINT64_MAX;
        uint16_t count = 0;
        uint16_t reserved = 0;
        uint32_t indices[8]{};
    };

    uint32_t holdOnset_ = 0;      // BASSMIDI-style onset ramp (frames) for hold mode, 0 = engine attack
    float holdGain_ = 1.0f;       // level of a hold-mode voice during its first ms
    uint32_t tuningEdo_ = 12u;
    bool releaseAll_ = false;     // BASSMIDI 2018: a note-off frees every generation on the key
    bool rejectPairing_ = false;  // see OutcomeTrack in NoteOn
    // Newborn level remembered per (channel, note, velocity) while nothing that
    // changes it has happened (levelEpoch_ counts controls, programs, bends,
    // sysex, resets, volume and bank changes), so a note the full pool would
    // skip anyway never builds its setups.  Only used with steal reject.
    struct NewbornMemo { uint32_t key; uint32_t epoch; float level; };
    static constexpr uint32_t kNewbornMemoBits = 14u;
    std::vector<NewbornMemo> newbornMemo_;
    uint32_t levelEpoch_ = 1u;
    bool lastNoteLaunched_ = false;
    std::vector<std::deque<uint8_t>> keyOutcome_;

    // Cached indices are LOCAL to the font's SF2Data.
    uint32_t ResolveRegions(uint32_t font, uint32_t preset, uint8_t note,
                            uint8_t velocity, const SFSampleRegion** out,
                            uint32_t capacity) {
        const Bank& bank = banks_[font];
        const uint64_t tag = (uint64_t(font) << 32u) |
                             (preset << 15u) | (uint32_t(note) << 7u) | velocity;
        uint32_t hash = uint32_t(tag) ^ (uint32_t(font) * 0x9e3779b1u);
        hash ^= hash >> 16u;
        hash *= 0x7feb352du;
        hash ^= hash >> 15u;
        RegionCacheEntry& cached = regionCache_[hash & 4095u];
        if (cached.tag == tag && cached.count <= 8u) {
            const uint32_t copied = (std::min)(uint32_t(cached.count), capacity);
            for (uint32_t i = 0; i < copied; ++i)
                out[i] = &bank.data->regions[cached.indices[i]];
            return cached.count;
        }
        const uint32_t count = sf2_find_regions(bank.data.get(), preset, note,
                                                velocity, out, capacity);
        if (count <= 8u && count <= capacity) {
            cached.tag = tag;
            cached.count = uint16_t(count);
            for (uint32_t i = 0; i < count; ++i)
                cached.indices[i] = uint32_t(out[i] - bank.data->regions);
        }
        return count;
    }

    void Prepare(const SF2Data* sf2, const SFSampleRegion& region,
                 PreparedRegion& prepared) {
        prepared = {};
        if (!sf2_validate_region(sf2, &region) ||
            region.sampleIndex >= sf2->sampleCount) return;
        const SF2Sample& sample = sf2->samples[region.sampleIndex];
        const int root = region.rootKey >= 0 ? region.rootKey : sample.originalPitch;
        prepared.bendScale =
            (region.scaleTuning ? region.scaleTuning : 100) / 100.0f;
        const float sampleRatio = float(sample.sampleRate ? sample.sampleRate : 44100) /
                                  float(engineRate_);
        for (uint32_t note = 0; note < kNoteCount; ++note) {
            const float semitones = (float(note) + region.coarseTune +
                                     region.fineTune / 100.0f - root) *
                                    prepared.bendScale;
            prepared.baseStep[note] = sampleRatio * powf(2.0f, semitones / 12.0f);
        }
        prepared.attenuation = (region.initialAttenuation > 0 ||
                                (sf2->isSfz && region.initialAttenuation != 0))
            ? InitialAttenuationToGain(float(region.initialAttenuation)) : 1.0f;
        prepared.sustain = (std::min)(
            1.0f, SustainAttenuationToGain(
                      (std::max)(0.0f, float(region.sustainVolEnv))));
        const auto samples = [&](int16_t timecents) {
            const float seconds = TimecentsToSeconds(timecents);
            return seconds > 0 ? uint32_t(seconds * engineRate_) : 0u;
        };
        prepared.delay = samples(region.delayVolEnv);
        prepared.hold = samples(region.holdVolEnv);
        const float attackSeconds = TimecentsToSeconds(region.attackVolEnv);
        const float decaySeconds = TimecentsToSeconds(region.decayVolEnv);
        prepared.attack = attackSeconds > 0.0001f
            ? uint32_t(attackSeconds * engineRate_) : 0u;
        prepared.decay = decaySeconds > 0.0001f
            ? uint32_t(decaySeconds * engineRate_) : 0u;
        prepared.decaySlope = 1.0f;
        if (prepared.decay) {
            const float slope = -9.226f / prepared.decay;
            prepared.decaySlope = expf(slope);
            if (prepared.sustain > 0 && prepared.sustain < 1)
                prepared.decay = uint32_t(logf(prepared.sustain) / slope);
        }
        const float releaseSeconds = TimecentsToSeconds(region.releaseVolEnv);
        prepared.releaseDecay = MakeReleaseDecay(releaseSeconds, engineRate_);
        prepared.release = MakeReleaseSamples(releaseSeconds, engineRate_);
        prepared.filter = PrepareVoiceLowPass(
            region.filterType, region.initialFilterFc, region.initialFilterQ,
            engineRate_);
        channels_.ComputeSoundFontPan(region.pan, prepared.panL, prepared.panR);
        prepared.valid = true;
    }

    bool Resolve(uint8_t channel, uint32_t& font, uint32_t& preset) {
        std::array<const SF2Data*, kMaxSoundFontStackEntries> data{};
        const uint16_t wantBank = channels_.GetBankMSB(channel);
        const uint8_t wantProgram = channels_.GetProgram(channel);
        const bool percussion = channels_.IsPercussion(channel);
        const int32_t askBank = percussion ? 128 : int32_t(wantBank);
        for (size_t i = 0; i < banks_.size(); ++i) {
            const Bank& b = banks_[i];
            // An entry restricted to a bank/program only answers those.
            if ((b.filterBank >= 0 && b.filterBank != askBank) ||
                (b.filterProgram >= 0 && b.filterProgram != wantProgram))
                continue;
            data[i] = b.data.get();
        }
        uint8_t found = 0;
        if (!ResolveStackPreset(data.data(), uint32_t(banks_.size()), nullptr, 0u,
                                int(channelFont_[channel]) - 1,
                                channels_.GetBankMSB(channel),
                                channels_.GetProgram(channel),
                                channels_.IsPercussion(channel), &found,
                                &preset))
            return false;
        font = found;
        return true;
    }

    void RefreshPreset(uint8_t channel) {
        uint32_t font = 0, preset = 0;
        if (Resolve(channel, font, preset)) {
            selFont_[channel] = uint8_t(font);
            channels_.SetSelectedPreset(channel, uint16_t(preset));
        } else {
            channels_.SetSelectedPreset(channel, UINT16_MAX);
        }
    }

    // Parses a whole stack, then swaps it in; the old one survives a failure.
    bool LoadBanks(const std::vector<std::wstring>& paths, std::string& error,
                   const std::vector<std::pair<int32_t, int32_t>>* filters =
                       nullptr) {
        if (paths.empty() || paths.size() > kMaxSoundFontStackEntries) {
            error = "SoundFont stack must hold 1..16 entries";
            return false;
        }
        std::vector<Bank> banks;
        const auto freeAll = [&] {
            for (Bank& b : banks) sf2_free(b.data.get());
        };
        if (filters && filters->size() != paths.size()) {
            error = "one filter per SoundFont is required";
            return false;
        }
        uint64_t frames = 0, regions = 0;
        for (size_t entry = 0; entry < paths.size(); ++entry) {
            const std::wstring& path = paths[entry];
            Bank bank;
            if (filters) {
                bank.filterBank = (*filters)[entry].first;
                bank.filterProgram = (*filters)[entry].second;
            }
            // () not {}: SF2Data holds 65536 SFSampleRegions with member
            // initializers; brace-init costs cl.exe ~3 s and far more memory
            // in every TU that sees it.
            bank.data.reset(new SF2Data());
            if (!soundfont_load(path.c_str(), bank.data.get())) {
                sf2_free(bank.data.get());
                freeAll();
                error = "failed to load SoundFont";
                return false;
            }
            sf2_build_regions(bank.data.get());
            if (oversample_ > 1u && !bank.data->regionOverflow &&
                !OversampleSF2(*bank.data, oversample_,
                               (std::max)(1u, std::thread::hardware_concurrency()))) {
                sf2_free(bank.data.get());
                freeAll();
                error = "cannot oversample the SoundFont";
                return false;
            }
            if (bank.data->regionOverflow || bank.data->regionCount == 0) {
                sf2_free(bank.data.get());
                freeAll();
                error = "SoundFont has no usable compiled regions";
                return false;
            }
            bank.sampleBase = uint32_t(frames);
            bank.regionBase = uint32_t(regions);
            frames += bank.data->sampleDataFrames;
            regions += bank.data->regionCount;
            banks.push_back(std::move(bank));
            if (frames >= UINT32_MAX - 8u) {
                freeAll();
                error = "SoundFont stack is too large";
                return false;
            }
        }
        std::vector<int16_t> samples, hilbert;
        try {
            samples.assign(size_t(frames) + 8u, int16_t{0});
            for (const Bank& b : banks)
                std::memcpy(samples.data() + b.sampleBase, b.data->sampleData,
                            size_t(b.data->sampleDataFrames) * sizeof(int16_t));
            // Standalone/offline must mean the same thing by "Analytic" as the
            // realtime driver: build the exact companion before rotation is
            // enabled; no allpass fallback for modes 1/2/4.
            if (rotationMode_ == 1u || rotationMode_ == 2u || rotationMode_ == 4u) {
                hilbert.assign(size_t(frames) + 8u, int16_t{0});
#if defined(SVMS_XP_COMPAT)
                const uint32_t buildThreads = 1u;
#else
                const uint32_t buildThreads = (std::min)(
                    16u, (std::max)(1u, std::thread::hardware_concurrency()));
#endif
                for (const Bank& b : banks)
                    BuildHilbertCompanion(*b.data, samples.data() + b.sampleBase,
                                          hilbert.data() + b.sampleBase,
                                          buildThreads);
            }
        } catch (...) {
            freeAll();
            error = "cannot allocate the SoundFont sample store";
            return false;
        }
        for (Bank& b : banks_) sf2_free(b.data.get());
        banks_ = std::move(banks);
        sampleData_ = std::move(samples);
        hilbertData_ = std::move(hilbert);
        sampleFrames_ = uint32_t(frames);
        prepared_.assign(size_t(regions), PreparedRegion{});
        ++levelEpoch_;
        for (RegionCacheEntry& e : regionCache_) e = RegionCacheEntry{};
        for (uint8_t& f : selFont_) f = 0;
        for (uint8_t& f : channelFont_) if (f > banks_.size()) f = 0;
        return true;
    }

    void RebuildPrepared() {
        ++levelEpoch_;
        for (const Bank& b : banks_) {
            std::vector<int16_t> codes(b.data->sampleCount, int16_t(-1));
            for (uint32_t i = 0; i < b.data->regionCount; ++i) {
                PreparedRegion& out = prepared_[b.regionBase + i];
                Prepare(b.data.get(), b.data->regions[i], out);
                const uint32_t si = b.data->regions[i].sampleIndex;
                if (!out.valid || si >= b.data->sampleCount ||
                    !b.data->sampleData) continue;
                if (codes[si] < 0) {
                    const SF2Sample& smp = b.data->samples[si];
                    codes[si] = smp.end > smp.start &&
                                smp.end <= b.data->sampleDataFrames
                        ? int16_t(MeasureSampleBandCode(
                              sampleData_.data() + b.sampleBase + smp.start,
                              smp.end - smp.start))
                        : int16_t(0);
                }
                out.halfBand = uint8_t(codes[si]);
            }
        }
    }

    // ---- release bus (ReleaseBus.h) ------------------------------------------------------------------------
    // The bus takes a note when it can reproduce it exactly: the channel is unbent with no vibrato and no pedal,
    // nothing but volume, expression and pan touches it between note-on and note-off, and the note's regions have
    // a kernel. Two ways in:
    // - same-block notes (note-off in the same render call and bus block, nothing else held on the key) never get
    //   a voice (BusNoteOn);
    // - other notes start as voices; at a note-off within kHandoffBlocks the bus takes the tail and the voices stop
    //   at t0 + 8 blocks (BusTrack, BusHandoff, BusRetire).
    // Everything else is a voice, as before.

    void StartBus() {
        const char* delay = std::getenv("SVMS_BUS_DELAY");
        bus_.Configure(busBlock_, busAheadOn_ && !(delay && !std::strcmp(delay, "0")) ? kBusDelayBlocks : 1);
        busDelay_ = bus_.Delay();
        if (busLine_[0].size() != busDelay_) {
            for (auto& line : busLine_) line.assign(busDelay_, 0.0f);
            busLinePos_ = 0;
        }
        bus_.SetParallel(&RenderScalar::ParallelFor, &renderer_, renderer_.GetRenderThreadCount());
        bus_.SetSharing(busShare_);
        keyHeld_.assign(size_t(kChannelCount) * kNoteCount, 0u);
        busOff_.assign(size_t(kChannelCount) * kNoteCount, UINT64_MAX);
        busPend_.assign(size_t(kChannelCount) * kNoteCount, BusPend{});
        busKeys_.assign(size_t(kChannelCount) * kNoteCount, BusKey{});
        busAheadFront_ = 0;   // a lookahead stays on (EnableBusLookahead); its pairing starts over
        StartBusKernels();
        busLate_.clear();
        busLateOrder_.clear();
        busKill_.clear();
        for (uint8_t channel = 0; channel < kChannelCount; ++channel) BusMix(channel);
    }

    // Adds the bus to the voices in left/right, which are at `frame`. Delayed (busDelay_): the voices come out of
    // the delay line busDelay_ frames late, and the bus is rendered at that frame (silence before frame 0).
    void RenderBus(float* left, float* right, uint32_t n, uint64_t frame) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!busDelay_) {
            bus_.Render(left, right, n, frame);
        } else {
            float* line[2] = {busLine_[0].data(), busLine_[1].data()};
            float* io[2] = {left, right};
            uint32_t pos = busLinePos_;
            for (uint32_t i = 0; i < n; ++i) {
                for (int s = 0; s < 2; ++s) std::swap(io[s][i], line[s][pos]);
                if (++pos == busDelay_) pos = 0;
            }
            busLinePos_ = pos;
            if (frame + n > busDelay_) {
                const uint32_t skip = frame < busDelay_ ? uint32_t(busDelay_ - frame) : 0u;
                bus_.Render(left + skip, right + skip, n - skip, frame + skip - busDelay_);
            }
        }
        busRenderNs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }

    // The kernel store, empty. With a lookahead the kernels keep level 0 (EnableBusLookahead).
    void StartBusKernels() {
        bus_.SetLevel0(busAheadOn_);
        busSets_.clear();
        busSetIds_.clear();
        busSetOf_.clear();
        // A kernel key names a font, a region set (busSets_) and a key. Inline mode: built on this thread, so the
        // set registry needs no lock.
        busKernels_.Start([this](uint64_t key, std::vector<bus::KernelSource>& sources) {
            const uint32_t set = uint32_t(key >> 8);
            if (set >= busSets_.size()) return false;
            sources.clear();
            for (uint32_t local : busSets_[set]) {
                sources.emplace_back();
                if (!ReleaseKernelSource(uint32_t(key >> 40), local, uint8_t(key), sources.back())) return false;
            }
            return true;
        }, bus_.Direct(), int(busBlock_), busBudget_, bus_.KernelLevels(), /*background=*/false,
           /*keepLevel0=*/busAheadOn_, bus_.FirstLevelBlocks());
    }

    // The kernel for a note the bus could take, checking what must hold for the note's whole life: one unit,
    // an unbent channel with no vibrato or pedal, 12-EDO, no rotation, no exclusive classes, no re-excite, and a
    // kernel for its region set. `layers` is the note's region count.
    const bus::ReleaseKernel* BusKernel(uint8_t channel, uint8_t note, uint8_t velocity, uint32_t count,
                                        uint64_t& kernelKey, uint32_t& layers, float& v) {
        if (count > 1u || note >= kNoteCount || channel >= kChannelCount) return nullptr;
        v = velGain_[velocity & 127u];
        if (v <= 0.0f || bendRatio_[channel] != 1.0f || tuningEdo_ != 12u || voices_.v.rot != nullptr ||
            !noteMergeHoist_ || reExcite_ || channels_.IsSustainActive(channel) ||
            channels_.GetParams()[channel].modDepth != 0.0f)
            return nullptr;
        const uint32_t preset = channels_.GetSelectedPreset(channel);
        const uint32_t font = selFont_[channel];
        if (font >= banks_.size() || preset >= banks_[font].data->presetCount) return nullptr;
        const Bank& bank = banks_[font];
        const SFSampleRegion* regions[8];
        const uint32_t n = ResolveRegions(font, preset, note, velocity, regions, 8);
        if (n == 0u || n > 8u) return nullptr;
        // One kernel for the note's whole region set (a stereo pair is one convolution, not two).
        const uint64_t tag = (uint64_t(font) << 32) | (uint64_t(preset) << 15) | (uint32_t(note) << 7) | velocity;
        auto known = busSetOf_.find(tag);
        if (known == busSetOf_.end()) {
            std::vector<uint32_t> locals(n);
            for (uint32_t i = 0; i < n; ++i) {
                locals[i] = uint32_t(regions[i] - bank.data->regions);
                const uint32_t index = bank.regionBase + locals[i];
                if (index >= prepared_.size() || !prepared_[index].valid) return nullptr;
            }
            auto id = busSetIds_.find(locals);
            if (id == busSetIds_.end()) {
                id = busSetIds_.emplace(locals, uint32_t(busSets_.size())).first;
                busSets_.push_back(locals);
            }
            known = busSetOf_.emplace(tag, id->second).first;
        }
        // A retire floor raised above the default shortens a voice's release to where it would reach the floor
        // (FloorLimitedReleaseSamples). That saves voice time; a bus tail costs nothing more to keep, so the bus
        // lets it ring to its natural end, as at the default floor. The difference is under the floor.
        kernelKey = bus::KernelKey(font, known->second, note);
        layers = n;
        return busKernels_.Get(kernelKey);
    }

    // A note that launched as a voice and could hand its tail to the bus at note-off: remember its generation.
    void BusTrack(uint8_t channel, uint8_t note, uint8_t velocity, uint32_t count) {
        if (!lastNoteLaunched_ || busAheadOn_) return;   // with the lookahead the bus has level 0: no handoffs
        uint64_t kernelKey = 0;
        uint32_t layers = 0;
        float v = 0.0f;
        const bus::ReleaseKernel* kernel = BusKernel(channel, note, velocity, count, kernelKey, layers, v);
        if (!kernel) return;
        const uint32_t generation = playIndex_ - 1u;
        busLate_[generation] = BusLate{busFrame_, kernelKey, kernel, v, busLive_[channel], channel, note,
                                       uint8_t(layers)};
        busLateOrder_.push_back({busFrame_, generation});
        // Candidates past the handoff deadline can never hand off.
        const uint64_t horizon = uint64_t(kHandoffBlocks + 2u) * busBlock_;
        while (!busLateOrder_.empty() && busLateOrder_.front().first + horizon < busFrame_) {
            busLate_.erase(busLateOrder_.front().second);
            busLateOrder_.pop_front();
        }
    }

    // At a note-off, before the engine releases the generation: if it is a tracked candidate whose channel stayed
    // clean, whose note-off came in time and whose layers all still sound, the bus takes its tail. The voices
    // play their release until a render call would pass t0 + D (BusRetire).
    void BusHandoff(uint8_t channel, uint8_t note) {
        if (releaseAll_ || busLate_.empty()) return;
        VoiceHandle handles[8];
        uint32_t generation = 0;
        const uint32_t n = voices_.NextReleasedLayers(channel, note, nestedNoteOff_, handles, 8, generation);
        if (n == 0u) return;
        const auto it = busLate_.find(generation);
        if (it == busLate_.end()) return;
        const BusLate late = it->second;
        busLate_.erase(it);
        const uint64_t held = busFrame_ - late.on;
        if (late.epoch != busLive_[channel] || n != late.layers || held > uint64_t(kHandoffBlocks) * busBlock_ ||
            held >= late.kernel->heldFrames || !bus_.Insertable(channel, late.on))
            return;
        bus_.AddImpulse(late.kernelKey, late.kernel, channel, late.on, uint32_t(held), late.v);
        BusKill kill{late.on, late.kernel, late.v, uint32_t(held), generation, channel, note, uint8_t(n), {}};
        for (uint32_t i = 0; i < n; ++i) kill.handles[i] = handles[i];
        busKill_.push_back(kill);
        ++busHanded_;
    }

    // At the start of a render call: handed-off voices whose call would pass t0 + D stop here, silently, and the
    // bus renders their release from this frame to t0 + D directly (the convolution takes over at t0 + D).
    void BusRetire(uint64_t frame, uint32_t frames) {
        for (size_t i = 0; i < busKill_.size();) {
            const BusKill& k = busKill_[i];
            if (frame + frames <= k.on + bus_.Direct()) { ++i; continue; }
            for (uint32_t l = 0; l < k.layers; ++l)
                voices_.RetireHandedOff(k.handles[l], k.generation, k.channel, k.note);
            bus_.AddDirect(k.kernel, k.channel, k.on, k.held, k.v, frame);
            busKill_[i] = busKill_.back();
            busKill_.pop_back();
        }
    }

    void BusMix(uint8_t channel) {
        if (!busOn_) return;
        const ChannelParamsSnapshot& cp = channels_.GetParams()[channel];
        bus_.SetMix(channel, busFrame_, cp.mixScaleLeft, cp.mixScaleRight);
    }

    // Pairs each note-on of the call with its note-off when nothing on the channel can change the note between,
    // within the bus's direct span: inside the call, or up to that far past it with a host's lookahead.
    void BusPlan(const RenderEvent* events, uint32_t count, uint64_t absoluteFrame) {
        busPlan_.clear();
        ++busCall_;
        const auto step = [&](RenderEventType type, uint8_t c, uint8_t data1, uint32_t n, uint64_t f) {
            switch (type) {
            case RenderEventType::NoteOn:
                if (data1 >= kNoteCount) break;
                busPend_[c * kNoteCount + data1] = n > 1u ? BusPend{} : BusPend{f, busCall_, busEpoch_[c]};
                break;
            case RenderEventType::NoteOff: {
                if (data1 >= kNoteCount) break;
                BusPend& p = busPend_[c * kNoteCount + data1];
                if (p.call == busCall_ && p.epoch == busEpoch_[c] && f - p.on < bus_.Direct())
                    busPlan_.push_back({uint32_t(c * kNoteCount + data1), p.on, f});
                p = BusPend{};
                break;
            }
            case RenderEventType::ControlChange:
                if (data1 != 7u && data1 != 10u && data1 != 11u) ++busEpoch_[c];
                break;
            default:
                ++busEpoch_[c];
                break;
            }
        };
        if (busAheadOn_) { FeedAhead(); return; }
        for (uint32_t i = 0; i < count; ++i) {
            const RenderEvent& e = events[i];
            step(e.type, uint8_t(e.channel & 15u), e.data1, e.count, absoluteFrame + e.frameOffset);
        }
        std::sort(busPlan_.begin(), busPlan_.end());
    }

    // With a host's lookahead: the engine's note-off pairing run ahead of it, each event once, when it first
    // appears. Each key keeps its open note-ons in order and a note-off ends the oldest (the newest with nested
    // note-off), as NoteOff does with voices; a bus note has no voice, so the voices left pair as they would.
    // A note whose note-off comes within the bus's direct span, with nothing between on its channel that can
    // change it, is a pair the bus can take at its note-on, whatever else the key holds.
    void FeedAhead() {
        for (size_t i = 0; i < busAheadCount_; ++i) {
            const RealtimeEvent& e = busAhead_[i];
            if (e.targetFrame < busAheadFront_) continue;   // fed with an earlier call
            const uint8_t status = uint8_t(e.message & 0xffu), c = status & 15u, type = status & 0xf0u;
            if (e.reset) {
                for (BusKey& k : busKeys_) k.open.clear();
                for (uint32_t& x : busEpoch_) ++x;
                continue;
            }
            if (IsInternalEngineMessage(e.message) || status < 0x80u || status >= 0xf0u) {
                for (uint32_t& x : busEpoch_) ++x;   // a barrier: nothing pairs across it
                continue;
            }
            if (type == 0x80u || type == 0x90u) {
                const uint32_t note = (e.message >> 8u) & 0xffu;
                if (note >= kNoteCount) continue;
                BusKey& k = busKeys_[c * kNoteCount + note];
                if (type == 0x90u && (e.message & 0x7f0000u)) { k.open.push_back({e.targetFrame, busEpoch_[c]}); continue; }
                if (k.open.empty()) continue;
                const BusOpen o = nestedNoteOff_ ? k.open.back() : k.open.front();
                if (nestedNoteOff_) k.open.pop_back(); else k.open.pop_front();
                if (o.epoch == busEpoch_[c] && e.targetFrame - o.on < bus_.Direct())
                    k.pairs.push_back({o.on, e.targetFrame});
                continue;
            }
            const uint32_t cc = (e.message >> 8u) & 0x7fu;
            if (type == 0xb0u && (cc == 7u || cc == 10u || cc == 11u)) continue;
            ++busEpoch_[c];
            if (type == 0xb0u && (cc == 120u || cc == 123u))   // every note on the channel ends (Control)
                for (uint32_t n = 0; n < kNoteCount; ++n) busKeys_[c * kNoteCount + n].open.clear();
        }
        if (busAheadCount_) busAheadFront_ = (std::max)(busAheadFront_, busAhead_[busAheadCount_ - 1].targetFrame + 1u);
    }

    bool BusNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, uint32_t count) {
        if (count > 1u || note >= kNoteCount || channel >= kChannelCount) return false;
        const uint64_t on = busFrame_;
        const uint32_t key = uint32_t(channel) * kNoteCount + note;
        uint64_t off = 0;
        if (busAheadOn_) {
            // Note-ons come in time order: a pair whose note-on is past (merged or refused) is dead, and is marked so
            // as the scan passes it; the dead ones in front are skipped for good, so with FIFO pairing the match is
            // the first one looked at.
            BusKey& k = busKeys_[key];
            auto& pairs = k.pairs;
            bool found = false;
            for (size_t i = k.head; i < pairs.size(); ++i) {
                uint64_t& first = pairs[i].first;
                if (first != kTaken && first < on) first = kTaken;
                if (first == kTaken) { if (i == k.head) ++k.head; continue; }
                if (first == on) { found = true; off = pairs[i].second; first = kTaken; if (i == k.head) ++k.head; break; }
            }
            if (k.head == pairs.size()) { pairs.clear(); k.head = 0; }
            else if (k.head > 64u && 2u * k.head > pairs.size()) {
                pairs.erase(pairs.begin(), pairs.begin() + std::ptrdiff_t(k.head));
                k.head = 0;
            }
            if (!found || !bus_.Open(channel, on)) return false;
        } else {
            if (keyHeld_[key] != 0u || !bus_.Open(channel, on)) return false;
            const auto it = std::lower_bound(busPlan_.begin(), busPlan_.end(), BusPair{key, on, 0u});
            if (it == busPlan_.end() || it->key != key || it->on != on) return false;
            off = it->off;
        }
        uint64_t kernelKey = 0;
        uint32_t layers = 0;
        float v = 0.0f;
        const bus::ReleaseKernel* kernel = BusKernel(channel, note, velocity, count, kernelKey, layers, v);
        if (!kernel || off - on >= kernel->heldFrames) return false;
        // With level 0 the convolution gives the held frames v A r^(tau - held + 1) and the correction takes it back:
        // a gain of r^(1 - held) on the convolution's rounding. Above 40 dB (a long note with a short release) the
        // note stays a voice.
        static const double kLog100 = std::log(100.0);
        if (busAheadOn_ && (double(off - on) - 1.0) * kernel->negLogDecay > kLog100)
            return false;
        bus_.AddNote(kernelKey, kernel, channel, on, uint32_t(off - on), v);
        channels_.NoteOn(channel, note, velocity);
        ++noteCalls_;
        ++notes_;
        ++busNotes_;
        ++keyHeld_[key];
        if (busAheadOn_) {
            auto& offs = busKeys_[key].offs;
            offs.push_back(off);
            std::push_heap(offs.begin(), offs.end(), std::greater<uint64_t>());
        }
        else busOff_[key] = off;
        return true;
    }

    // The note-off of a bus note: its release is already in the bus.
    bool BusNoteOff(uint8_t channel, uint8_t note) {
        if (note >= kNoteCount || channel >= kChannelCount) return false;
        const uint32_t key = uint32_t(channel) * kNoteCount + note;
        if (busAheadOn_) {
            // The earliest pending note-off; earlier ones never came (they can no longer match).
            auto& offs = busKeys_[key].offs;
            const auto pop = [&] { std::pop_heap(offs.begin(), offs.end(), std::greater<uint64_t>()); offs.pop_back(); };
            while (!offs.empty() && offs.front() < busFrame_) pop();
            if (offs.empty() || offs.front() != busFrame_) return false;
            pop();
        } else {
            if (busOff_[key] != busFrame_) return false;
            busOff_[key] = UINT64_MAX;
        }
        if (keyHeld_[key] != 0u) --keyHeld_[key];
        channels_.NoteOff(channel, note);
        return true;
    }

    void UpdateNoteMergeHoist() {
        noteMergeHoist_ = true;
        for (const Bank& b : banks_)
            for (uint32_t i = 0; i < b.data->regionCount; ++i)
                if (b.data->regions[i].exclusiveClass != 0 ||
                    b.data->regions[i].offByClass != 0) {
                    noteMergeHoist_ = false;
                    return;
                }
    }

    // `multiplicity` > 1 launches one voice standing for that many exact
    // duplicates (NoteMerge.h). Rotating voices each own a random phase
    // angle, so they launch one by one instead.
    void NoteOn(uint8_t channel, uint8_t note, uint8_t velocity,
                uint32_t multiplicity = 1u) {
        if (multiplicity > 1u && voices_.v.rot != nullptr) {
            for (uint32_t unit = 0u; unit < multiplicity; ++unit)
                NoteOn(channel, note, velocity, 1u);
            return;
        }
#if defined(_MSC_VER)
        const uint64_t totalBegin = __rdtsc();
#endif
        // Steal-reject pairing: every note-on, launched or not, queues its outcome so
        // the matching note-off releases a voice only when this note got one.
        struct OutcomeTrack {
            StandaloneSynth* self; uint8_t channel, note;
            ~OutcomeTrack() {
                if (self->rejectPairing_ && note < kNoteCount && channel < kChannelCount)
                    self->keyOutcome_[channel * kNoteCount + note].push_back(
                        self->lastNoteLaunched_ ? 1u : 0u);
            }
        } outcomeTrack{this, channel, note};
        lastNoteLaunched_ = false;
        ++noteCalls_;
        if (note >= kNoteCount) return;
        if (!keyHeld_.empty() && channel < kChannelCount)
            keyHeld_[channel * kNoteCount + note] += multiplicity;
        if (velGain_[velocity & 127u] <= 0.0f) return;  // velocity ignore gate
        channels_.NoteOn(channel, note, velocity);
        uint32_t preset = channels_.GetSelectedPreset(channel);
        uint32_t font = selFont_[channel];
        if (preset >= banks_[font].data->presetCount) {
            if (!Resolve(channel, font, preset)) {
                ++missingPresets_;
                return;
            }
            selFont_[channel] = uint8_t(font);
            channels_.SetSelectedPreset(channel, uint16_t(preset));
        }
        const Bank& bank = banks_[font];
#if defined(_MSC_VER)
        const uint64_t resolveBegin = __rdtsc();
#endif
        const SFSampleRegion* regions[512];
        const bool edo31 = tuningEdo_ == 31u && !channels_.IsPercussion(channel);
        const uint8_t regionNote = MidiRegionKey(note, edo31);
        uint32_t count = ResolveRegions(font, preset, regionNote, velocity, regions, 512);
#if defined(_MSC_VER)
        dispatchProfile.resolve += __rdtsc() - resolveBegin;
#endif
        if (!count || count > 512) {
            // Region fallback: resolve against the widest-coverage preset so
            // incomplete instruments stay audible (see SF2Data docs).
            const uint16_t fb = bank.data->fallbackPresetIndex;
            if (fb < bank.data->presetCount && fb != preset)
                count = ResolveRegions(font, fb, regionNote, velocity, regions, 512);
            if (!count || count > 512) {
                ++missingRegions_;
                return;
            }
            ++fallbackRegions_;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t regionIndex = bank.regionBase +
                uint32_t(regions[i] - bank.data->regions);
            if (regionIndex >= prepared_.size() || !prepared_[regionIndex].valid) {
                ++invalidRegions_;
                return;
            }
        }
        if (playIndex_ == 0 || playIndex_ >= UINT32_MAX - 1) playIndex_ = 1;
        const uint32_t generation = playIndex_++;
        NewbornMemo* memo = nullptr;
        const uint32_t memoKey = (uint32_t(channel) << 14) |
            (uint32_t(note) << 7) | (velocity & 127u);
        if (multiplicity == 1u && voices_.StealRejectEnabled()) {
            if (newbornMemo_.empty())
                newbornMemo_.assign(size_t(1) << kNewbornMemoBits,
                                    NewbornMemo{UINT32_MAX, 0u, 0.0f});
            memo = &newbornMemo_[(memoKey * 2654435761u) >>
                                 (32u - kNewbornMemoBits)];
            if (memo->key == memoKey && memo->epoch == levelEpoch_ &&
                voices_.RejectByNewbornLevel(count, memo->level)) {
#if defined(_MSC_VER)
                dispatchProfile.noteOnTotal += __rdtsc() - totalBegin;
                ++dispatchProfile.noteOnCalls;
#endif
                ++notes_;
                return;
            }
        }
        const float velocityGain = velGain_[velocity & 127u];
        const float bend = channels_.GetPitchBendSemitones(channel) +
            sysexMasterFineTune_ + sysexMasterTranspose_;
#if defined(_MSC_VER)
        const uint64_t configureBegin = __rdtsc();
#endif
        for (uint32_t i = 0; i < count; ++i) {
            const SFSampleRegion& region = *regions[i];
            const uint32_t localRegion = uint32_t(regions[i] - bank.data->regions);
            const PreparedRegion& prepared = prepared_[bank.regionBase + localRegion];
            const float bendRatio = prepared.bendScale == 1.0f
                ? bendRatio_[channel]
                : powf(2.0f, bend * prepared.bendScale / 12.0f);
            const float gain = velocityGain * prepared.attenuation * holdGain_;
            VoiceConfiguration& voice = launchSetups_[i];
            voice = VoiceConfiguration{};
            voice.sampleStart = bank.sampleBase + uint32_t(region.startOffset);
            voice.sampleEnd = bank.sampleBase + uint32_t(region.endOffset);
            voice.loopStart = bank.sampleBase + uint32_t(region.loopStartOffset);
            voice.loopEnd = bank.sampleBase + uint32_t(region.loopEndOffset);
            voice.loopMode = region.loopMode;
            voice.playIndex = generation;
            voice.delaySamples = prepared.delay;
            voice.holdSamples = prepared.hold;
            const uint32_t attackFrames = holdOnset_ ? holdOnset_ : prepared.attack;
            voice.attackSamples = attackFrames;
            voice.decaySamples = prepared.decay;
            voice.releaseSamples = prepared.release;
            const float baseStep = RetunePhaseStep(prepared.baseStep[note], note, prepared.bendScale, edo31);
            voice.phaseStep = baseStep * bendRatio;
            voice.basePhaseStep = baseStep;
            voice.pitchBendScale = prepared.bendScale;
            voice.initialGain = gain;
            voice.sustainLevel = prepared.sustain;
            voice.attackGainStep = attackFrames ? gain / attackFrames : 0;
            voice.decaySlope = prepared.decaySlope;
            voice.releaseDecay = prepared.releaseDecay;
            voice.gainLeft = prepared.panL;
            voice.gainRight = prepared.panR;
            voice.filterA0 = prepared.filter.a0;
            voice.filterB1 = prepared.filter.b1;
            voice.filterB2 = prepared.filter.b2;
            voice.filterEnabled = prepared.filter.enabled;
            voice.presetIndex = uint16_t(preset);
            voice.regionIndex = uint16_t(localRegion);
            voice.halfBand = prepared.halfBand;
            voice.exclusiveClass = region.exclusiveClass > 0
                ? uint16_t(region.exclusiveClass) : 0u;
            voice.offByClass = region.offByClass < 0
                ? UINT16_MAX
                : (region.offByClass > 0
                    ? uint16_t(region.offByClass) : 0u);
            voice.sampleBacked = 1;
        }
        if (memo != nullptr && !(memo->key == memoKey && memo->epoch == levelEpoch_)) {
            bool memoizable = true;
            for (uint32_t i = 0; i < count; ++i)
                memoizable = memoizable && launchSetups_[i].exclusiveClass == 0u;
            if (memoizable)
                *memo = NewbornMemo{memoKey, levelEpoch_,
                    voices_.QueryNewbornLevel(launchSetups_, count,
                                              channels_.GetParams()[channel], 1u)};
        }
        // Production launch path: one transaction resolves the batched
        // victim selection once for the whole region group (layers feed on
        // freed sibling slots), instead of one full steal-transaction per
        // region. Same victims, same configuration, same commit semantics.
#if defined(_MSC_VER)
        const uint64_t allocBegin = __rdtsc();
#endif
        VoiceHandle handles[512];
#if defined(SVMS_NOTE_TRACE)
        const size_t traceFirst = g_noteTrace ? g_noteTrace->launches.size() : 0u;
        const uint64_t traceBegin = __rdtsc();
#endif
        const bool launched = voices_.LaunchVoiceGroup(
            channel, note, velocity, launchSetups_, count, generation,
            channels_.GetParams()[channel], handles, 0u, multiplicity);
#if defined(SVMS_NOTE_TRACE)
        if (g_noteTrace) g_noteTrace->StampCycles(traceFirst, __rdtsc() - traceBegin);
#endif
        if (launched) { physicalLaunches_ += count; lastNoteLaunched_ = true; }
#if defined(_MSC_VER)
        dispatchProfile.alloc += __rdtsc() - allocBegin;
        dispatchProfile.configure += __rdtsc() - configureBegin;
        dispatchProfile.noteOnTotal += __rdtsc() - totalBegin;
        ++dispatchProfile.noteOnCalls;
#endif
        ++notes_;
    }

    void NoteOff(uint8_t channel, uint8_t note) {
#if defined(_MSC_VER)
        const uint64_t offBegin = __rdtsc();
#endif
        const bool sustain = channels_.IsSustainActive(channel);
        channels_.NoteOff(channel, note);
        if (!keyHeld_.empty() && note < kNoteCount && channel < kChannelCount &&
            keyHeld_[channel * kNoteCount + note] != 0u)
            --keyHeld_[channel * kNoteCount + note];
        if (rejectPairing_ && note < kNoteCount && channel < kChannelCount) {
            std::deque<uint8_t>& outcomes = keyOutcome_[channel * kNoteCount + note];
            if (!outcomes.empty()) {
                const bool launched = outcomes.front() != 0u;
                outcomes.pop_front();
                if (!launched) return;  // that note was skipped: nothing to release
            }
        }
        if (busOn_ && !sustain && note < kNoteCount && channel < kChannelCount) BusHandoff(channel, note);
        // One note-off releases one unit: the whole generation, or one copy
        // of a merged voice.
        voices_.NoteOffOldestPlayIndices(channel, note, releaseAll_ ? UINT32_MAX : 1u, sustain, 0u, nestedNoteOff_);
#if defined(_MSC_VER)
        dispatchProfile.noteOffTotal += __rdtsc() - offBegin;
        ++dispatchProfile.noteOffCalls;
#endif
    }

    void Control(uint8_t channel, uint8_t controller, uint8_t value) {
        ++levelEpoch_;
        if (controller != 7u && controller != 10u && controller != 11u) ++busLive_[channel & 15u];
#if defined(_MSC_VER)
        const uint64_t controlBegin = __rdtsc();
#endif
        const bool sustain = channels_.IsSustainActive(channel);
        channels_.ControlChange(channel, controller, value);
        if (controller == 0 || controller == 32) RefreshPreset(channel);
        if (controller == 64 && value < 64) ReleaseSustain(channel);
        if (controller == 120) voices_.SilenceChannelImmediate(channel);
        else if (controller == 123) voices_.ReleaseChannel(channel, 0);
        if (busOn_ && (controller == 120 || controller == 123)) {
            // Every held voice on the channel is gone or released; bus tails stop with all sound off.
            std::fill(keyHeld_.begin() + channel * kNoteCount, keyHeld_.begin() + (channel + 1) * kNoteCount, 0u);
            for (uint32_t n = 0; n < kNoteCount; ++n) busKeys_[channel * kNoteCount + n].offs.clear();
            if (controller == 120) {
                bus_.Cut(channel, busFrame_);
                busKill_.erase(std::remove_if(busKill_.begin(), busKill_.end(),
                                              [&](const BusKill& k) { return k.channel == channel; }),
                               busKill_.end());
            }
        }
        else if (controller == 121 && sustain) ReleaseSustain(channel);
        if (controller == 121) Bend(channel, 0, 64);
        else if (controller == 6 || controller == 38 ||
                 controller == 96 || controller == 97)
            RefreshChannelPitch(channel);
#if defined(_MSC_VER)
        const uint64_t rebuildBegin = __rdtsc();
#endif
        if (controller == 7 || controller == 10 || controller == 11 ||
            controller == 64 || controller == 121 || controller == 1) {
            channels_.RebuildChannel(channel, cfg_, float(engineRate_));
#if defined(_MSC_VER)
            dispatchProfile.controlRebuild += __rdtsc() - rebuildBegin;
#endif
            if (controller == 7 || controller == 10 || controller == 11 ||
                controller == 121) {
#if defined(_MSC_VER)
                const uint64_t mixBegin = __rdtsc();
#endif
                voices_.MarkChannelMixStale(
                    channel, channels_.GetParams()[channel]);
                BusMix(channel);
#if defined(_MSC_VER)
                dispatchProfile.controlMix += __rdtsc() - mixBegin;
#endif
            }
            if (controller == 1 || controller == 121) {
                // Whole-voice pre-pass: report the post-rebuild modulation
                // depth as a vibrato row-op (exact-frame LFO gating).
                voices_.MarkChannelVibratoDepth(
                    channel, channels_.GetParams()[channel].modDepth);
            }
        }
#if defined(_MSC_VER)
        dispatchProfile.controlTotal += __rdtsc() - controlBegin;
        ++dispatchProfile.controlCalls;
#endif
    }

    void ReleaseSustain(uint8_t channel) {
        voices_.ForEachChannelActive(channel, [&](VoiceHandle handle) {
            if (voices_.v.heldBySustain[handle]) {
                voices_.v.heldBySustain[handle] = 0;
                voices_.StartRelease(handle);
            }
        });
    }

    void Program(uint8_t channel, uint8_t program) {
        ++levelEpoch_;
        ++busLive_[channel & 15u];
        const uint8_t oldProgram = channels_.GetProgram(channel);
        channels_.ProgramChange(channel, program);
        uint32_t font = 0, preset = 0;
        if (Resolve(channel, font, preset)) {
            selFont_[channel] = uint8_t(font);
            channels_.SetSelectedPreset(channel, uint16_t(preset));
        } else {
            // No such preset in the stack: the channel goes silent (no placeholder instrument).
            (void)oldProgram;
            channels_.SetSelectedPreset(channel, UINT16_MAX);
        }
    }

    void Bend(uint8_t channel, uint8_t low, uint8_t high) {
        ++levelEpoch_;
        ++busLive_[channel & 15u];
#if defined(_MSC_VER)
        const uint64_t bendBegin = __rdtsc();
#endif
        channels_.PitchBend(channel, int16_t((high << 7u) | low));
        RefreshChannelPitch(channel);
#if defined(_MSC_VER)
        dispatchProfile.bendTotal += __rdtsc() - bendBegin;
        ++dispatchProfile.bendCalls;
#endif
    }

    void RefreshChannelPitch(uint8_t channel) {
        const float semitones = channels_.GetPitchBendSemitones(channel) +
            sysexMasterFineTune_ + sysexMasterTranspose_;
        // Production route: the whole-voice pre-pass installs a bend-op hook
        // on the VoiceManager, which turns this inline rewrite into a
        // recorded channel op so batched bend events qualify for the
        // whole-voice renderer. With no hook set this performs the exact
        // same per-voice rewrite the inline loop did.
        voices_.ApplyChannelBendRatio(channel, semitones);
        bendRatio_[channel] = powf(2.0f, semitones / 12.0f);
    }

    void RefreshAllPitch() {
        for (uint8_t channel = 0; channel < kChannelCount; ++channel)
            RefreshChannelPitch(channel);
    }

    void DispatchInternal(uint32_t message, uint64_t frame) {
        ++levelEpoch_;
        for (uint32_t& e : busLive_) ++e;   // tuning, reset: no handoff across them
        if (message == kInternalResetMessage) {
            ResetAll(frame);
            return;
        }
        if ((message & 0xffffc000u) == kInternalMasterVolumeTag) {
            sysexMasterVolume_ = static_cast<float>(message & 0x3fffu) /
                16383.0f;
            channels_.SetMasterVolume(master_ * sysexMasterVolume_);
            channels_.RebuildCache(cfg_, static_cast<float>(engineRate_));
            for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
                voices_.MarkChannelMixStale(
                    channel, channels_.GetParams()[channel]);
                BusMix(channel);
            }
            return;
        }
        if ((message & 0xffffc000u) == kInternalMasterFineTuneTag) {
            sysexMasterFineTune_ = static_cast<float>(
                static_cast<int32_t>(message & 0x3fffu) - 8192) / 8192.0f;
            RefreshAllPitch();
            return;
        }
        if ((message & 0xffffff80u) == kInternalMasterTransposeTag) {
            sysexMasterTranspose_ = static_cast<float>(
                static_cast<int32_t>(message & 0x7fu) - 64);
            RefreshAllPitch();
            return;
        }
        if ((message & 0xff000000u) == kInternalRhythmPartTag) {
            const uint8_t channel = static_cast<uint8_t>(
                (message >> 8u) & 0x0fu);
            channels_.SetRhythmPart(
                channel, static_cast<uint8_t>(message & 0x03u));
            RefreshPreset(channel);
        }
    }

#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
    // The GPU stage is a pure per-voice sample synthesizer.  Two CPU-only
    // behaviors are not (yet) modeled there; the scalar path is bit-exact
    // for both, so fall back instead of drift:
    //   - phase rotation: stateful per-voice Hilbert DSP (voices_.v.rot);
    //   - vibrato LFO: phaseIncs refreshed on a fixed 64-frame cadence
    //     (AdvanceVibratoSpan) whenever any channel has mod depth.
    bool GpuFallbackNeeded() const {
        if (voices_.v.rot != nullptr) return true;
        const ChannelParamsSnapshot* params = channels_.GetParams();
        for (uint32_t channel = 0; channel < kChannelCount; ++channel)
            if (params[channel].modDepth > 0.0f) return true;
        return false;
    }
#endif

    uint32_t rate_ = 0;
    uint32_t engineRate_ = 0;   // the voice engine's rate (rate_ or half of it)
    bool uniformHalf_ = false;
    std::vector<float> halfBlock_[2];   // [history 32][half-rate samples]
    std::vector<float> halfOut_[2];
    std::vector<float> halfAcc_[2];
    std::vector<RenderEvent> halfEvents_;
    std::vector<RenderEvent> halfPending_;
    std::vector<float> halfFifo_[2];
    size_t halfFifoHead_ = 0;
    uint32_t maxVoices_ = 0;
    uint32_t playIndex_ = 0;
    uint64_t renderCycles_ = 0;
    float master_ = 0;
    float sysexMasterVolume_ = 1.0f;
    float sysexMasterFineTune_ = 0.0f;
    float sysexMasterTranspose_ = 0.0f;
    float bendRatio_[kChannelCount]{};
    uint64_t notes_ = 0, noteCalls_ = 0, physicalLaunches_ = 0;
    uint64_t missingPresets_ = 0;
    uint64_t missingRegions_ = 0, invalidRegions_ = 0, fallbackRegions_ = 0;
    struct Bank {
        std::unique_ptr<SF2Data> data;
        uint32_t sampleBase = 0;   // offset into the concatenated sample store
        uint32_t regionBase = 0;   // offset into prepared_
        int32_t filterBank = -1;     // -1 = any, 128 = percussion
        int32_t filterProgram = -1;  // -1 = any
    };
    std::vector<Bank> banks_;
    uint8_t channelFont_[kChannelCount]{};  // 0 = automatic, N = banks_[N-1]
    uint8_t selFont_[kChannelCount]{};      // bank the channel's preset is in
    uint32_t rotationMode_ = 0;
    std::vector<int16_t> sampleData_;
    std::vector<int16_t> hilbertData_;
    // Release bus state (see StartBus). keyHeld_ counts note-ons not yet matched by a note-off per channel and
    // key; busOff_ is a bus note's note-off frame; busPlan_ the current call's note pairs.
    bool busOn_ = false;
    size_t busBudget_ = 0;
    uint32_t busBlock_ = 0;
    static constexpr int kBusDelayBlocks = 32;   // a delayed bus's first level (ReleaseBus::Configure)
    uint32_t busDelay_ = 0;                      // frames: the bus renders this far behind, the voices wait
    bool busShare_ = std::getenv("SVMS_BUS_NO_SHARE") == nullptr;   // off: one group per channel (A/B runs)
    std::vector<float> busLine_[2];              // the voices' delay line
    uint32_t busLinePos_ = 0;
    uint64_t busFrame_ = 0;   // output frame of the event being applied
    uint64_t busNotes_ = 0;
    uint32_t busCall_ = 0;
    uint32_t busEpoch_[kChannelCount]{};
    struct BusPend { uint64_t on = 0; uint32_t call = 0, epoch = 0; };
    struct BusPair {
        uint32_t key; uint64_t on, off;
        bool operator<(const BusPair& o) const { return key != o.key ? key < o.key : on < o.on; }
    };
    std::vector<BusPend> busPend_;
    std::vector<BusPair> busPlan_;
    const RealtimeEvent* busAhead_ = nullptr;   // the host's lookahead for this call (SetBusLookahead)
    size_t busAheadCount_ = 0;
    bool busAheadOn_ = false;     // pairs come from the lookahead (FeedAhead), not the call
    uint64_t busAheadFront_ = 0;  // lookahead events before this frame are fed
    struct BusOpen { uint64_t on; uint32_t epoch; };
    static constexpr uint64_t kTaken = UINT64_MAX;   // BusKey::pairs: taken, or its note-on is past
    struct BusKey {
        std::deque<BusOpen> open;                          // note-ons not yet ended, in order (lookahead time)
        std::vector<std::pair<uint64_t, uint64_t>> pairs;  // (on, off) the bus can take, from `head` on; taken or past
                                                           // ones have on = kTaken
        size_t head = 0;
        std::vector<uint64_t> offs;                        // note-offs of bus notes still to come, a min-heap
    };
    std::vector<BusKey> busKeys_;
    std::vector<uint32_t> keyHeld_;
    std::vector<uint64_t> busOff_;
    uint64_t busPlanNs_ = 0, busRenderNs_ = 0;   // report only
    std::vector<std::vector<uint32_t>> busSets_;           // set id -> its regions (bank-local indices)
    std::map<std::vector<uint32_t>, uint32_t> busSetIds_;  // regions -> set id
    std::unordered_map<uint64_t, uint32_t> busSetOf_;      // font, preset, key, velocity -> set id
    // Handoff (late notes). A candidate's note-off must come within kHandoffBlocks of its note-on, which leaves
    // its voices a block to stop in before t0 + 8 blocks.
    static constexpr uint32_t kHandoffBlocks = 6u;
    uint32_t busLive_[kChannelCount]{};   // bumped by anything that changes how a channel's notes sound
    struct BusLate {
        uint64_t on, kernelKey; const bus::ReleaseKernel* kernel; float v; uint32_t epoch;
        uint8_t channel, note, layers;
    };
    struct BusKill {
        uint64_t on; const bus::ReleaseKernel* kernel; float v; uint32_t held, generation;
        uint8_t channel, note, layers; VoiceHandle handles[8];
    };
    std::unordered_map<uint32_t, BusLate> busLate_;          // generation -> candidate
    std::deque<std::pair<uint64_t, uint32_t>> busLateOrder_; // (note-on frame, generation), oldest first
    std::vector<BusKill> busKill_;                           // handed off, voices still playing
    uint64_t busHanded_ = 0;
    bus::ReleaseKernelStore busKernels_;
    bus::ReleaseBus bus_;
    uint32_t sampleFrames_ = 0;
    std::vector<PreparedRegion> prepared_;
    VoiceManager voices_;
    ChannelCache channels_;
#if defined(SVMS_GPU_STAGE)
    std::unique_ptr<GpuStage> gpuStage_; // declared before renderer_ so it is destroyed after it
#endif
    RenderScalar renderer_;
    bool gpuEnabled_ = false;
#if !defined(SVMS_XP_COMPAT) && defined(_WIN32)
    GpuSynth gpuSynth_;
#endif
    RuntimeConfigSnapshot cfg_{};
    float velGain_[128]{};
    // Driver FDN reverb, applied before the limiter exactly like
    // Driver::RenderCallback (interleaved scratch round trip).
    std::unique_ptr<ReverbState> reverb_;  // large; heap, only when enabled
    bool reverbOn_ = false;
    bool monoDownmix_ = false;
    void ApplyMono(float* left, float* right, uint32_t frames) const {
        if (!monoDownmix_) return;
        for (uint32_t i = 0u; i < frames; ++i) {
            const float mid = 0.5f * (left[i] + right[i]);
            left[i] = mid;
            right[i] = mid;
        }
    }
    std::vector<float> reverbScratch_;
    void ApplyReverb(float* left, float* right, uint32_t frames) {
        if (!reverbOn_ || frames == 0u) return;
        if (reverbScratch_.size() < size_t(frames) * 2u)
            reverbScratch_.resize(size_t(frames) * 2u);
        float* s = reverbScratch_.data();
        for (uint32_t i = 0; i < frames; ++i) { s[2u * i] = left[i]; s[2u * i + 1u] = right[i]; }
        reverb_->Process(s, frames, 2u);
        for (uint32_t i = 0; i < frames; ++i) { left[i] = s[2u * i]; right[i] = s[2u * i + 1u]; }
    }
public:
    bool ReverbActive() const { return reverbOn_; }
private:
    PostHighPass3Hz postHighPass_{};
    LimiterRouterState limiter_{};
    RegionCacheEntry regionCache_[4096]{};
};

} // namespace svms

#endif
