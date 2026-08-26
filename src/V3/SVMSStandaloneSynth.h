#ifndef SVMS_STANDALONE_SYNTH_H
#define SVMS_STANDALONE_SYNTH_H

#include "SVMSChannelCache.h"
#include "SVMSConfig.h"
#include "SVMSEnvelope.h"
#include "SVMSEventCompile.h"
#include "SVMSLimiter.h"
#include "SVMSPostFilter.h"
#include "SVMSRenderScalar.h"
#include "SVMSSoundFont.h"
#include "SVMSVoiceManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace svms {

struct StandaloneSynthConfig {
    std::wstring soundfont;
    uint32_t sampleRate = 44100;
    uint32_t maxVoices = 4096;
    uint32_t renderThreads = 1;
    uint32_t maxBlockFrames = 2048;
    float masterVolume = 1.0f;
    bool limiterEnabled = true;
    LimiterAlgorithm limiterAlgorithm = LimiterAlgorithm::Classic;
    float limiterThreshold = 0.95f;
    float limiterLookaheadMs = 3.0f;
    float limiterAttackMs = 0.5f;
    float limiterReleaseMs = 100.0f;
    RenderBackend backend = RenderBackend::AVX512; // sentinel: automatic
};

// Platform-neutral owner for the existing V3 synthesis core. Event ordering
// and absolute-frame placement remain the responsibility of the caller.
class StandaloneSynth {
public:
    bool Initialize(const StandaloneSynthConfig& config, std::string& error) {
        rate_ = config.sampleRate;
        maxVoices_ = config.maxVoices;
        master_ = config.masterVolume;
        sf2_.reset(new SF2Data{});
        if (!sf2_load(config.soundfont.c_str(), sf2_.get())) {
            error = "failed to load SoundFont";
            return false;
        }
        sf2_build_regions(sf2_.get());
        if (sf2_->regionOverflow || sf2_->regionCount == 0) {
            error = "SoundFont has no usable compiled regions";
            return false;
        }
        sampleData_.resize(sf2_->sampleDataFrames);
        for (uint32_t i = 0; i < sf2_->sampleDataFrames; ++i)
            sampleData_[i] = sf2_->sampleData[i] / 32768.0f;
        prepared_.resize(sf2_->regionCount);
        if (!voices_.Initialize(maxVoices_, rate_)) {
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
                                              config.maxBlockFrames)) {
            error = "cannot initialize render workers";
            return false;
        }
        channels_.Reset();
        channels_.SetMasterVolume(master_);
        cfg_ = {master_, 1.0f, 0.0f, 0, false, false, false, false, false,
                false, InterpolationMode::Linear, FilterType::None,
                PanLaw::ConstantPower, true};
        channels_.RebuildCache(cfg_, static_cast<float>(rate_));
        for (uint32_t i = 0; i < sf2_->regionCount; ++i)
            Prepare(sf2_->regions[i], prepared_[i]);
        for (uint8_t channel = 0; channel < kChannelCount; ++channel)
            RefreshPreset(channel);
        if (config.backend != RenderBackend::AVX512 &&
            !renderer_.SetRenderBackend(config.backend)) {
            error = "requested render backend is unsupported on this CPU";
            return false;
        }
        for (float& ratio : bendRatio_) ratio = 1.0f;
        sysexMasterVolume_ = 1.0f;
        sysexMasterFineTune_ = 0.0f;
        sysexMasterTranspose_ = 0.0f;
        postHighPass_.Initialize(rate_);
        EngineConfig limiterConfig{};
        limiterConfig.limiterEnabled = config.limiterEnabled;
        limiterConfig.limiterAlgorithm = config.limiterAlgorithm;
        limiterConfig.limiterThreshold = config.limiterThreshold;
        limiterConfig.limiterLookaheadMs = config.limiterLookaheadMs;
        limiterConfig.limiterAttackMs = config.limiterAttackMs;
        limiterConfig.limiterReleaseMs = config.limiterReleaseMs;
        limiter_.Configure(rate_, limiterConfig);
        return true;
    }

    ~StandaloneSynth() {
        if (sf2_) sf2_free(sf2_.get());
    }

    StandaloneSynth() = default;
    StandaloneSynth(const StandaloneSynth&) = delete;
    StandaloneSynth& operator=(const StandaloneSynth&) = delete;

    void Dispatch(uint32_t message, uint64_t frame) {
        voices_.SetCurrentFrame(frame);
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
        std::fill(left, left + frameCount, 0.0f);
        std::fill(right, right + frameCount, 0.0f);
        renderer_.RenderBlock(voices_, channels_, sampleData_.data(),
                              uint32_t(sampleData_.size()), left, right,
                              frameCount, cfg_, nullptr, 0, true,
                              absoluteFrame);
        limiter_.ProcessPlanar(left, right, frameCount, postHighPass_);
    }

    void ReleaseAll() {
        for (uint8_t channel = 0; channel < kChannelCount; ++channel)
            voices_.ReleaseChannel(channel, 0);
    }

    void ResetAll(uint64_t absoluteFrame) {
        voices_.SetCurrentFrame(absoluteFrame);
        for (uint8_t channel = 0; channel < kChannelCount; ++channel)
            voices_.SilenceChannelImmediate(channel);
        channels_.Reset();
        channels_.SetMasterVolume(master_);
        sysexMasterVolume_ = 1.0f;
        sysexMasterFineTune_ = 0.0f;
        sysexMasterTranspose_ = 0.0f;
        channels_.RebuildCache(cfg_, static_cast<float>(rate_));
        for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
            bendRatio_[channel] = 1.0f;
            RefreshPreset(channel);
        }
    }

    uint32_t Active() const { return voices_.GetActiveCount(); }
    uint32_t Tails() const { return voices_.GetStealTailCount(); }
    uint32_t Steals() const { return voices_.stealCount_; }
    uint32_t Free() const { return maxVoices_ - voices_.GetActiveCount(); }
    uint64_t NoteCalls() const { return noteCalls_; }
    uint64_t MatchedNotes() const { return notes_; }
    uint64_t MissingPresets() const { return missingPresets_; }
    uint64_t MissingRegions() const { return missingRegions_; }
    uint64_t InvalidRegions() const { return invalidRegions_; }
    uint64_t FallbackRegions() const { return fallbackRegions_; }
    const char* Backend() const { return renderer_.GetRenderBackendName(); }

private:
    struct PreparedRegion {
        float baseStep[kNoteCount];
        float bendScale, attenuation, sustain, decaySlope, releaseDecay;
        float panL, panR;
        uint32_t delay, hold, attack, decay, release;
        bool valid;
    };

    struct RegionCacheEntry {
        uint32_t tag = UINT32_MAX;
        uint16_t count = 0;
        uint16_t reserved = 0;
        uint32_t indices[8]{};
    };

    uint32_t ResolveRegions(uint32_t preset, uint8_t note, uint8_t velocity,
                            const SFSampleRegion** out, uint32_t capacity) {
        const uint32_t tag = (preset << 14u) | (uint32_t(note) << 7u) | velocity;
        uint32_t hash = tag;
        hash ^= hash >> 16u;
        hash *= 0x7feb352du;
        hash ^= hash >> 15u;
        RegionCacheEntry& cached = regionCache_[hash & 4095u];
        if (cached.tag == tag && cached.count <= 8u) {
            const uint32_t copied = (std::min)(uint32_t(cached.count), capacity);
            for (uint32_t i = 0; i < copied; ++i)
                out[i] = &sf2_->regions[cached.indices[i]];
            return cached.count;
        }
        const uint32_t count = sf2_find_regions(sf2_.get(), preset, note,
                                                velocity, out, capacity);
        if (count <= 8u && count <= capacity) {
            cached.tag = tag;
            cached.count = uint16_t(count);
            for (uint32_t i = 0; i < count; ++i)
                cached.indices[i] = uint32_t(out[i] - sf2_->regions);
        }
        return count;
    }

    void Prepare(const SFSampleRegion& region, PreparedRegion& prepared) {
        prepared = {};
        if (!sf2_validate_region(sf2_.get(), &region) ||
            region.sampleIndex >= sf2_->sampleCount) return;
        const SF2Sample& sample = sf2_->samples[region.sampleIndex];
        const int root = region.rootKey >= 0 ? region.rootKey : sample.originalPitch;
        prepared.bendScale =
            (region.scaleTuning ? region.scaleTuning : 100) / 100.0f;
        const float sampleRatio = float(sample.sampleRate ? sample.sampleRate : 44100) /
                                  float(rate_);
        for (uint32_t note = 0; note < kNoteCount; ++note) {
            const float semitones = (float(note) + region.coarseTune +
                                     region.fineTune / 100.0f - root) *
                                    prepared.bendScale;
            prepared.baseStep[note] = sampleRatio * powf(2.0f, semitones / 12.0f);
        }
        prepared.attenuation = region.initialAttenuation > 0
            ? InitialAttenuationToGain(float(region.initialAttenuation)) : 1.0f;
        prepared.sustain = (std::min)(
            1.0f, SustainAttenuationToGain(
                      (std::max)(0.0f, float(region.sustainVolEnv))));
        const auto samples = [&](int16_t timecents) {
            const float seconds = TimecentsToSeconds(timecents);
            return seconds > 0 ? uint32_t(seconds * rate_) : 0u;
        };
        prepared.delay = samples(region.delayVolEnv);
        prepared.hold = samples(region.holdVolEnv);
        const float attackSeconds = TimecentsToSeconds(region.attackVolEnv);
        const float decaySeconds = TimecentsToSeconds(region.decayVolEnv);
        prepared.attack = attackSeconds > 0.0001f
            ? uint32_t(attackSeconds * rate_) : 0u;
        prepared.decay = decaySeconds > 0.0001f
            ? uint32_t(decaySeconds * rate_) : 0u;
        prepared.decaySlope = 1.0f;
        if (prepared.decay) {
            const float slope = -9.226f / prepared.decay;
            prepared.decaySlope = expf(slope);
            if (prepared.sustain > 0 && prepared.sustain < 1)
                prepared.decay = uint32_t(logf(prepared.sustain) / slope);
        }
        const float releaseSeconds = TimecentsToSeconds(region.releaseVolEnv);
        prepared.releaseDecay = MakeReleaseDecay(releaseSeconds, rate_);
        prepared.release = MakeReleaseSamples(releaseSeconds, rate_);
        channels_.ComputeSoundFontPan(region.pan, prepared.panL, prepared.panR);
        prepared.valid = true;
    }

    bool Resolve(uint8_t channel, uint32_t& preset) {
        return sf2_resolve_preset(sf2_.get(), channels_.GetBankMSB(channel),
                                  channels_.GetProgram(channel),
                                  channels_.IsPercussion(channel),
                                  &preset);
    }

    void RefreshPreset(uint8_t channel) {
        uint32_t preset = 0;
        channels_.SetSelectedPreset(
            channel, Resolve(channel, preset) ? uint16_t(preset) : UINT16_MAX);
    }

    void NoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
        ++noteCalls_;
        if (note >= kNoteCount) return;
        channels_.NoteOn(channel, note, velocity);
        uint32_t preset = channels_.GetSelectedPreset(channel);
        if (preset >= sf2_->presetCount) {
            if (!Resolve(channel, preset)) {
                ++missingPresets_;
                return;
            }
            channels_.SetSelectedPreset(channel, uint16_t(preset));
        }
        const SFSampleRegion* regions[512];
        uint32_t count = ResolveRegions(preset, note, velocity, regions, 512);
        if (!count || count > 512) {
            // Region fallback: resolve against the widest-coverage preset so
            // incomplete instruments stay audible (see SF2Data docs).
            const uint16_t fb = sf2_->fallbackPresetIndex;
            if (fb < sf2_->presetCount && fb != preset)
                count = ResolveRegions(fb, note, velocity, regions, 512);
            if (!count || count > 512) {
                ++missingRegions_;
                return;
            }
            ++fallbackRegions_;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t regionIndex = uint32_t(regions[i] - sf2_->regions);
            if (regionIndex >= prepared_.size() || !prepared_[regionIndex].valid) {
                ++invalidRegions_;
                return;
            }
        }
        if (playIndex_ == 0 || playIndex_ >= UINT32_MAX - 1) playIndex_ = 1;
        const uint32_t generation = playIndex_++;
        VoiceHandle handles[512];
        uint32_t made = 0;
        for (; made < count; ++made) {
            handles[made] = voices_.AllocateVoiceOrSteal(
                channel, note, velocity, nullptr, count == 1);
            if (handles[made] == kInvalidVoice) {
                for (uint32_t index = 0; index < made; ++index)
                    voices_.RetireVoice(handles[index]);
                return;
            }
        }
        const float velocityGain = float(velocity) * float(velocity) /
                                   (127.0f * 127.0f);
        const float bend = channels_.GetPitchBendSemitones(channel) +
            sysexMasterFineTune_ + sysexMasterTranspose_;
        for (uint32_t i = 0; i < count; ++i) {
            const SFSampleRegion& region = *regions[i];
            const uint32_t regionIndex = uint32_t(regions[i] - sf2_->regions);
            const PreparedRegion& prepared = prepared_[regionIndex];
            const float bendRatio = prepared.bendScale == 1.0f
                ? bendRatio_[channel]
                : powf(2.0f, bend * prepared.bendScale / 12.0f);
            const float gain = velocityGain * prepared.attenuation;
            VoiceConfiguration voice{};
            voice.sampleStart = uint32_t(region.startOffset);
            voice.sampleEnd = uint32_t(region.endOffset);
            voice.loopStart = uint32_t(region.loopStartOffset);
            voice.loopEnd = uint32_t(region.loopEndOffset);
            voice.loopMode = region.loopMode;
            voice.playIndex = generation;
            voice.delaySamples = prepared.delay;
            voice.holdSamples = prepared.hold;
            voice.attackSamples = prepared.attack;
            voice.decaySamples = prepared.decay;
            voice.releaseSamples = prepared.release;
            voice.phaseStep = prepared.baseStep[note] * bendRatio;
            voice.basePhaseStep = prepared.baseStep[note];
            voice.pitchBendScale = prepared.bendScale;
            voice.initialGain = gain;
            voice.sustainLevel = prepared.sustain;
            voice.attackGainStep = prepared.attack ? gain / prepared.attack : 0;
            voice.decaySlope = prepared.decaySlope;
            voice.releaseDecay = prepared.releaseDecay;
            voice.gainLeft = prepared.panL;
            voice.gainRight = prepared.panR;
            voice.presetIndex = uint16_t(preset);
            voice.regionIndex = uint16_t(regionIndex);
            voice.sampleBacked = 1;
            voices_.ConfigureVoice(handles[i], voice,
                                   channels_.GetParams()[channel], count == 1);
        }
        ++notes_;
    }

    void NoteOff(uint8_t channel, uint8_t note) {
        const bool sustain = channels_.IsSustainActive(channel);
        channels_.NoteOff(channel, note);
        const uint32_t playIndex = voices_.FindOldestPlayIndex(channel, note);
        if (playIndex != UINT32_MAX)
            voices_.NoteOffPlayIndex(channel, note, playIndex, sustain, 0);
    }

    void Control(uint8_t channel, uint8_t controller, uint8_t value) {
        const bool sustain = channels_.IsSustainActive(channel);
        channels_.ControlChange(channel, controller, value);
        if (controller == 0 || controller == 32) RefreshPreset(channel);
        if (controller == 64 && value < 64) ReleaseSustain(channel);
        if (controller == 120) voices_.SilenceChannelImmediate(channel);
        else if (controller == 123) voices_.ReleaseChannel(channel, 0);
        else if (controller == 121 && sustain) ReleaseSustain(channel);
        if (controller == 121) Bend(channel, 0, 64);
        else if (controller == 6 || controller == 38 ||
                 controller == 96 || controller == 97)
            RefreshChannelPitch(channel);
        if (controller == 7 || controller == 10 || controller == 11 ||
            controller == 64 || controller == 121) {
            channels_.RebuildChannel(channel, cfg_, float(rate_));
            if (controller == 7 || controller == 10 || controller == 11 ||
                controller == 121)
                voices_.RefreshMixGainsForChannel(
                    channel, channels_.GetParams()[channel]);
        }
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
        const uint8_t oldProgram = channels_.GetProgram(channel);
        channels_.ProgramChange(channel, program);
        uint32_t preset = 0;
        if (Resolve(channel, preset))
            channels_.SetSelectedPreset(channel, uint16_t(preset));
        else
            channels_.ProgramChange(channel, oldProgram);
    }

    void Bend(uint8_t channel, uint8_t low, uint8_t high) {
        channels_.PitchBend(channel, int16_t((high << 7u) | low));
        RefreshChannelPitch(channel);
    }

    void RefreshChannelPitch(uint8_t channel) {
        const float semitones = channels_.GetPitchBendSemitones(channel) +
            sysexMasterFineTune_ + sysexMasterTranspose_;
        const float common = powf(2.0f, semitones / 12.0f);
        bendRatio_[channel] = common;
        voices_.ForEachChannelActive(channel, [&](VoiceHandle voice) {
            const float scale = voices_.v.pitchBendScales[voice];
            voices_.v.phaseIncs[voice] = voices_.v.basePhaseIncs[voice] *
                (scale == 1 ? common : powf(2.0f, semitones * scale / 12.0f));
        });
    }

    void RefreshAllPitch() {
        for (uint8_t channel = 0; channel < kChannelCount; ++channel)
            RefreshChannelPitch(channel);
    }

    void DispatchInternal(uint32_t message, uint64_t frame) {
        if (message == kInternalResetMessage) {
            ResetAll(frame);
            return;
        }
        if ((message & 0xffffc000u) == kInternalMasterVolumeTag) {
            sysexMasterVolume_ = static_cast<float>(message & 0x3fffu) /
                16383.0f;
            channels_.SetMasterVolume(master_ * sysexMasterVolume_);
            channels_.RebuildCache(cfg_, static_cast<float>(rate_));
            for (uint8_t channel = 0; channel < kChannelCount; ++channel)
                voices_.RefreshMixGainsForChannel(
                    channel, channels_.GetParams()[channel]);
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

    uint32_t rate_ = 0;
    uint32_t maxVoices_ = 0;
    uint32_t playIndex_ = 0;
    float master_ = 0;
    float sysexMasterVolume_ = 1.0f;
    float sysexMasterFineTune_ = 0.0f;
    float sysexMasterTranspose_ = 0.0f;
    float bendRatio_[kChannelCount]{};
    uint64_t notes_ = 0, noteCalls_ = 0, missingPresets_ = 0;
    uint64_t missingRegions_ = 0, invalidRegions_ = 0, fallbackRegions_ = 0;
    std::unique_ptr<SF2Data> sf2_;
    std::vector<float> sampleData_;
    std::vector<PreparedRegion> prepared_;
    VoiceManager voices_;
    ChannelCache channels_;
    RenderScalar renderer_;
    RuntimeConfigSnapshot cfg_{};
    PostHighPass3Hz postHighPass_{};
    LimiterRouterState limiter_{};
    RegionCacheEntry regionCache_[4096]{};
};

} // namespace svms

#endif
