// Part of VoiceManager.h; included from there inside namespace svms. Not standalone.

inline void VoiceManager::SetVoiceSample(VoiceHandle handle, uint32_t start, uint32_t end,
                                          uint32_t loopStart, uint32_t loopEnd, uint8_t loopMode,
                                          float phaseStep, uint8_t sb) {
    if (handle >= maxVoices_) return;
    EnsureMaterialized(handle);
    v.sampleStart[handle] = start;
    v.loopMode[handle]    = loopMode;
    v.phaseIncs[handle]   = phaseStep;
    v.basePhaseIncs[handle] = phaseStep;
    v.pitchBendScales[handle] = 1.0f;
    v.sampleBacked[handle] = sb;

    // Precomputed render constants (region bounds relative to sampleStart).
    // The per-sample loop reads these directly instead of recomputing them
    // from the absolute SF2 offsets every sample.
    uint32_t relE = (end > start) ? (end - start) : 0u;
    uint32_t relS = (loopStart > start) ? (loopStart - start) : 0u;
    uint32_t relL = (loopEnd > start) ? (loopEnd - start) : 0u;
    v.relEnd[handle]    = relE;
    v.relLoopS[handle]  = relS;
    v.relLoopE[handle]  = relL;
    v.relLoopSF[handle] = static_cast<float>(relS);
    v.relLoopEF[handle] = static_cast<float>(relL);
    // Mirrors ShouldLoopSVMS() for a freshly-allocated (Active) voice.
    v.loopEnabled[handle] = ((loopMode == 1 || loopMode == 3) &&
                             loopEnd > loopStart + 1u) ? 1 : 0;

    // Start every voice at the exact beginning of its SoundFont region.
    // Render-correct engines retrigger chopped notes from the same sample
    // position; randomizing this phase smears the repeated waveform and
    // destroys the characteristic coherent buzz of dense retriggers.
    v.phases[handle] = 0.0f;
    RefreshRenderClass(handle);
}

inline void VoiceManager::SetVoicePitchBase(VoiceHandle handle,
                                             float basePhaseStep,
                                             float pitchBendScale) {
    if (handle >= maxVoices_) return;
    v.basePhaseIncs[handle] = basePhaseStep;
    v.pitchBendScales[handle] = pitchBendScale;
}

inline void VoiceManager::SetVoiceSoundFontIdentity(VoiceHandle handle,
                                                      uint16_t presetIndex,
                                                      uint16_t regionIndex) {
    if (handle >= maxVoices_) return;
    v.presetIndex[handle] = presetIndex;
    v.regionIndex[handle] = regionIndex;
}

inline void VoiceManager::SetVoicePlayIndex(VoiceHandle handle,
                                             uint32_t playIndex) {
    if (handle >= maxVoices_) return;
    UnlinkPlayGroup(handle);
    v.playIndex[handle] = playIndex;
    if (playIndex == UINT32_MAX) return;
    if (lastLinkedPlayIndex_ == playIndex &&
        lastLinkedPlayVoice_ < maxVoices_ &&
        v.state[lastLinkedPlayVoice_] != static_cast<uint8_t>(VoiceState::Free) &&
        v.playIndex[lastLinkedPlayVoice_] == playIndex) {
        playGroupPrev_[handle] = static_cast<int32_t>(lastLinkedPlayVoice_);
        playGroupNext_[lastLinkedPlayVoice_] = static_cast<int32_t>(handle);
    }
    lastLinkedPlayIndex_ = playIndex;
    lastLinkedPlayVoice_ = handle;
}

inline uint32_t VoiceManager::FindOldestPlayIndex(uint8_t channel,
                                                   uint8_t note) const {
    if (channel >= kChannelCount || note >= kNoteCount) return UINT32_MAX;
    const int32_t oldest = channelKeyVoiceOldest_[channel][note];
    if (oldest < 0) return UINT32_MAX;
    const uint32_t handle = static_cast<uint32_t>(oldest);
    return v.state[handle] == static_cast<uint8_t>(VoiceState::Active) &&
        !v.heldBySustain[handle] ? v.playIndex[handle] : UINT32_MAX;
}

inline void VoiceManager::StartReleaseForPlayIndex(uint8_t channel,
                                                    uint8_t note,
                                                    uint32_t playIndex) {
    if (playIndex == UINT32_MAX) return;
    int32_t current = channelKeyVoiceHead_[channel][note];
    while (current >= 0) {
        const VoiceHandle handle = static_cast<VoiceHandle>(current);
        current = v.nextChannelKeyVoice[handle];
        if (v.state[handle] != static_cast<uint8_t>(VoiceState::Free) &&
            v.playIndex[handle] == playIndex) {
            StartRelease(handle);
        }
    }
}

inline void VoiceManager::NoteOffPlayIndex(uint8_t channel, uint8_t note,
                                            uint32_t playIndex, bool sustain,
                                            uint32_t blockOffset) {
    if (channel >= kChannelCount || note >= kNoteCount || playIndex == UINT32_MAX)
        return;
    // Generations are inserted as adjacent layers at the head. The tail is
    // the oldest outstanding generation, so ordinary note-off is O(layers)
    // instead of walking every same-key retrigger in the pool.
    int32_t current = channelKeyVoiceOldest_[channel][note];
    if (current >= 0 && v.playIndex[static_cast<uint32_t>(current)] == playIndex) {
        while (current >= 0) {
            const VoiceHandle handle = static_cast<VoiceHandle>(current);
            if (v.playIndex[handle] != playIndex) break;
            current = v.prevChannelKeyVoice[handle];
            if (sustain || v.heldBySostenuto[handle]) {
                if (sustain) v.heldBySustain[handle] = 1u;
                // A pedal-held generation has received its note-off and must
                // not mask the next retrigger. Pedal release uses the channel
                // active index, so key-chain membership is unnecessary.
                UnlinkChannelKey(handle);
            } else {
                v.releaseStartInBlock[handle] = blockOffset;
                StartRelease(handle);
            }
        }
        return;
    }

    // Preserve the public helper's behavior for uncommon callers that name a
    // non-oldest generation explicitly.
    current = channelKeyVoiceHead_[channel][note];
    while (current >= 0) {
        const VoiceHandle handle = static_cast<VoiceHandle>(current);
        current = v.nextChannelKeyVoice[handle];
        if (v.state[handle] == static_cast<uint8_t>(VoiceState::Free) ||
            v.playIndex[handle] != playIndex) {
            continue;
        }
        if (sustain || v.heldBySostenuto[handle]) {
            if (sustain) v.heldBySustain[handle] = 1u;
            UnlinkChannelKey(handle);
        } else {
            v.releaseStartInBlock[handle] = blockOffset;
            StartRelease(handle);
        }
    }
}

inline void VoiceManager::SetVoiceEnvelope(VoiceHandle handle, float initialGain,
                                            float sustainLevel, uint32_t delaySamples,
                                            uint32_t holdSamples, uint32_t attackSamples,
                                            uint32_t decaySamples, float attackGainStep,
                                            float decaySlope, float releaseDecay,
                                            uint32_t releaseSamples) {
    if (handle >= maxVoices_) return;
    EnsureMaterialized(handle);
    v.targetGain[handle] = initialGain;
    v.sustainLevel[handle] = sustainLevel * initialGain;
    v.delaySamplesRemaining[handle]  = delaySamples;
    v.holdSamplesRemaining[handle]   = holdSamples;
    v.attackSamplesRemaining[handle] = attackSamples;
    v.decaySamplesRemaining[handle]  = decaySamples;
    v.attackGainStep[handle] = attackGainStep;
    v.decaySlope[handle]     = decaySlope;
    v.releaseDecay[handle]   = releaseDecay;
    // UINT32_MAX preserves threshold-based behavior for synthetic/offline
    // callers that provide only a multiplier.  Live SF2 voices always pass
    // the exact parsed segment length.
    v.releaseSamplesRemaining[handle] = releaseSamples > 0u
        ? FloorLimitedReleaseSamples(releaseSamples, releaseDecay, initialGain)
        : UINT32_MAX;
    v.currentGain[handle]    = 0.0f;

    if (delaySamples > 0)
        v.envelopeStage[handle] = 4;
    else if (holdSamples > 0)
        v.envelopeStage[handle] = 0;
    else if (attackSamples > 0)
        v.envelopeStage[handle] = 1;
    else if (decaySamples > 0) {
        // SF2 regions commonly have an instantaneous attack followed by a
        // non-zero decay.  Decay must begin at the note's initial level;
        // leaving currentGain at its allocation value of zero makes the
        // entire region silent (it immediately clamps to its sustain level).
        v.envelopeStage[handle] = 2;
        v.currentGain[handle] = initialGain;
    } else {
        v.envelopeStage[handle] = 3;
        v.currentGain[handle] = initialGain;
    }
    RefreshRenderClass(handle);
}

inline void VoiceManager::SetVoiceGain(VoiceHandle handle, float left, float right) {
    if (handle >= maxVoices_) return;
    v.gainLeft[handle]  = left;
    v.gainRight[handle] = right;
}

inline void VoiceManager::RefreshMixGain(VoiceHandle handle, const ChannelParamsSnapshot& cp) {
    if (handle >= maxVoices_) return;
    EnsureMaterialized(handle);
    v.mixGainL[handle] = v.gainLeft[handle] * cp.mixScaleLeft;
    v.mixGainR[handle] = v.gainRight[handle] * cp.mixScaleRight;
    v.stealOutputGain[handle] = std::sqrt(
        v.mixGainL[handle] * v.mixGainL[handle] +
        v.mixGainR[handle] * v.mixGainR[handle]);
    v.renderGainL[handle] = v.currentGain[handle] * v.mixGainL[handle];
    v.renderGainR[handle] = v.currentGain[handle] * v.mixGainR[handle];
    UpdateStealCandidate(handle);
}

inline void VoiceManager::RefreshMixGains(const ChannelParamsSnapshot* chParams) {
    ++staticEpoch_;
    bool stableTreeDirty = false;
    for (uint32_t ai = 0; ai < activeCount_; ++ai) {
        uint32_t i = activeList_[ai];
        const ChannelParamsSnapshot& cp = chParams[v.channel[i]];
        FoldVoiceMixGains(static_cast<VoiceHandle>(i), cp.mixScaleLeft,
                          cp.mixScaleRight, stableTreeDirty, true);
    }
    if (stableTreeDirty) RebuildStableWinnerTree();
    // This eager full refresh supersedes any pending batched-fold marks.
    for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
        channelMixScaleL_[channel] = chParams[channel].mixScaleLeft;
        channelMixScaleR_[channel] = chParams[channel].mixScaleRight;
    }
    channelMixStealMask_ = 0u;
    channelMixDirtyMask_ = 0u;
}

inline void VoiceManager::MarkChannelMixStale(
    uint8_t channel, const ChannelParamsSnapshot& cp) {
    if (channel >= kChannelCount) return;
    channelMixScaleL_[channel] = cp.mixScaleLeft;
    channelMixScaleR_[channel] = cp.mixScaleRight;
    if (mixOpHook_) {
        // Whole-voice pre-pass: the per-voice fold runs at the exact event
        // frame inside the owning worker.  No masks are set here — a
        // launch-entry steal fold would rewrite mixGain of pre-CC voices
        // before the pre-pass captures them as ghosts, baking the new
        // scales into their whole audible window.  The renderer refreshes
        // the affected channels' steal candidates at block end instead
        // (FoldStealGainsForChannels), after all ghost captures are done.
        mixOpHook_(channel, cp.mixScaleLeft, cp.mixScaleRight,
                   rowOpUserData_);
        return;
    }
    channelMixStealMask_ |= (uint64_t(1) << channel);
    channelMixDirtyMask_ |= (uint64_t(1) << channel);
}

inline void VoiceManager::ApplyChannelBendRatio(uint8_t channel,
                                                float bendSemitones) {
    if (channel >= kChannelCount) return;
    const float commonRatio = powf(2.0f, bendSemitones / 12.0f);
    if (bendOpHook_) {
        bendOpHook_(channel, bendSemitones, commonRatio, rowOpUserData_);
        return;
    }
    ++staticEpoch_;
    ForEachChannelActive(channel, [&](VoiceHandle handle) {
        EnsureMaterialized(handle);
        const float scale = v.pitchBendScales[handle];
        const float ratio = scale == 1.0f
            ? commonRatio : powf(2.0f, bendSemitones * scale / 12.0f);
        v.phaseIncs[handle] = v.basePhaseIncs[handle] * ratio;
    });
}

inline void VoiceManager::FoldVoiceMixGains(
    VoiceHandle handle, float mixScaleLeft, float mixScaleRight,
    bool& stableTreeDirty, bool writeRenderGain) {
    const uint32_t i = handle;
    EnsureMaterialized(handle);
    v.mixGainL[i] = v.gainLeft[i] * mixScaleLeft;
    v.mixGainR[i] = v.gainRight[i] * mixScaleRight;
    v.stealOutputGain[i] = std::sqrt(
        v.mixGainL[i] * v.mixGainL[i] +
        v.mixGainR[i] * v.mixGainR[i]);
    if (writeRenderGain) {
        v.renderGainL[i] = v.currentGain[i] * v.mixGainL[i];
        v.renderGainR[i] = v.currentGain[i] * v.mixGainR[i];
    }
    if (stealHeapValid_ && stealCandidateDeferred_[i] == 0u &&
        IsStableStealCandidate(static_cast<VoiceHandle>(i)) &&
        IsLinkedActiveVoice(static_cast<VoiceHandle>(i)) &&
        stealWinnerTree_[stealTreeLeafBase_ + i] ==
            stealStableKey_[i]) {
        const float score = ComputeStableStealKey(
            static_cast<VoiceHandle>(i));
        stealStableKey_[i] = EncodeStableWinnerKey(
            score, activePosition_[i]);
        stealWinnerTree_[stealTreeLeafBase_ + i] = stealStableKey_[i];
        stableTreeDirty = true;
    } else {
        UpdateStealCandidate(static_cast<VoiceHandle>(i));
    }
}

inline void VoiceManager::FoldMask(uint64_t mask, bool writeRenderGain) {
    for (uint32_t channel = 0; channel < kChannelCount; ++channel) {
        const uint64_t bit = uint64_t(1) << channel;
        if ((mask & bit) == 0u) continue;
        const float mixScaleLeft = channelMixScaleL_[channel];
        const float mixScaleRight = channelMixScaleR_[channel];
        bool stableTreeDirty = false;
        ForEachChannelActive(static_cast<uint8_t>(channel),
            [&](VoiceHandle voice) {
                FoldVoiceMixGains(voice, mixScaleLeft, mixScaleRight,
                                  stableTreeDirty, writeRenderGain);
            });
        if (stableTreeDirty) RebuildStableWinnerTree();
    }
}

inline void VoiceManager::FoldStaleChannelMixGains() {
    uint64_t pending = channelMixStealMask_ | channelMixDirtyMask_;
    if (pending == 0u) return;
    channelMixStealMask_ = 0u;
    channelMixDirtyMask_ = 0u;
    ++staticEpoch_;
    FoldMask(pending, true);
}

inline void VoiceManager::FoldStaleChannelMixGainsForSteal() {
    uint64_t pending = channelMixStealMask_;
    if (pending == 0u) return;
    channelMixStealMask_ = 0u;
    FoldMask(pending, false);
}

inline void VoiceManager::FoldStealGainsForChannels(uint64_t channelMask) {
    if (kChannelCount < 64u)
        channelMask &= (uint64_t(1) << kChannelCount) - 1u;
    if (channelMask == 0u) return;
    FoldMask(channelMask, false);
}

inline void VoiceManager::RefreshMixGainsForChannel(
    uint8_t channel, const ChannelParamsSnapshot& cp) {
    if (channel >= kChannelCount) return;
    channelMixScaleL_[channel] = cp.mixScaleLeft;
    channelMixScaleR_[channel] = cp.mixScaleRight;
    channelMixStealMask_ &= ~(uint64_t(1) << channel);
    channelMixDirtyMask_ &= ~(uint64_t(1) << channel);
    ++staticEpoch_;
    bool stableTreeDirty = false;
    ForEachChannelActive(channel, [&](VoiceHandle voice) {
        FoldVoiceMixGains(voice, cp.mixScaleLeft, cp.mixScaleRight,
                          stableTreeDirty, true);
    });
    if (stableTreeDirty) RebuildStableWinnerTree();
}

inline void VoiceManager::EndVoicesForChannelKey(uint8_t channel, uint8_t note,
                                                   uint32_t blockOffset) {
    // Fixed fast release (100ms) regardless of pool pressure.
    // BASSMIDI uses SF2-defined release — we use a consistent fast release
    // so panic/sound-off always sounds the same.
    float releaseDecay = MakeReleaseDecay(0.100f, sampleRate_);
    const uint32_t releaseSamples = MakeReleaseSamples(0.100f, sampleRate_);

    int32_t idx = channelKeyVoiceHead_[channel][note];
    while (idx >= 0) {
        int32_t next = v.nextChannelKeyVoice[idx];
        if (v.state[idx] != static_cast<uint8_t>(VoiceState::Free)) {
            if (v.state[idx] == static_cast<uint8_t>(VoiceState::Active)) {
                EnsureMaterialized(static_cast<VoiceHandle>(idx));
                v.releaseDecay[idx] = releaseDecay;
                v.releaseSamplesRemaining[idx] = releaseSamples;
                v.releaseStartInBlock[idx] = blockOffset;
                StartRelease(static_cast<VoiceHandle>(idx));
            }
        }
        idx = next;
    }
}

inline void VoiceManager::SilenceChannelImmediate(uint8_t channel) {
    if (channel >= kChannelCount) return;
    // A stolen tail can belong to a different channel than the replacement
    // occupying its slot, so inspect the independent dense tail list.
    uint32_t tailPosition = 0u;
    while (tailPosition < stealTailCount_) {
        const uint32_t idx = stealTailList_[tailPosition];
        if (v.stealTailFramesRemaining[idx] != 0 &&
            v.stealTailChannel[idx] == channel) {
            // Whole-voice pre-pass: record the kill frame first — the tail's
            // fields survive UnlinkStealTail so the block render can emit
            // its audio up to that exact frame.
            if (silenceTailHook_)
                silenceTailHook_(idx, silenceHookUserData_);
            v.stealTailFramesRemaining[idx] = 0;
            UnlinkStealTail(static_cast<VoiceHandle>(idx));
            continue;
        }
        ++tailPosition;
    }
    while (channelActiveCount_[channel] > 0u) {
        const uint32_t idx = LastChannelActive(channel);
        assert(idx != kInvalidVoice);
        // Tail ownership is independent from the active slot and was handled
        // by channel identity above.
        // Whole-voice pre-pass: capture the exact pre-kill state as a ghost
        // BEFORE the bookkeeping retires the voice (hook is null otherwise).
        if (silenceVoiceHook_) {
            EnsureMaterialized(static_cast<VoiceHandle>(idx));
            silenceVoiceHook_(static_cast<VoiceHandle>(idx),
                              silenceHookUserData_);
        }
        v.stealFadeInFramesRemaining[idx] = 0;
        RetireVoice(static_cast<VoiceHandle>(idx));
    }
}

inline void VoiceManager::ReleaseChannel(uint8_t channel, uint32_t blockOffset) {
    if (channel >= kChannelCount) return;
    ForEachChannelActive(channel, [&](VoiceHandle voice) {
        const uint32_t idx = voice;
        if (v.state[idx] == static_cast<uint8_t>(VoiceState::Free)) return;
        v.heldBySustain[idx] = 0;
        v.heldBySostenuto[idx] = 0;
        v.releaseStartInBlock[idx] = blockOffset;
        StartRelease(static_cast<VoiceHandle>(idx));
    });
}

inline void VoiceManager::CaptureSostenuto(uint8_t channel) {
    if (channel >= kChannelCount) return;
    ForEachChannelActive(channel, [&](VoiceHandle handle) {
        if (v.state[handle] != static_cast<uint8_t>(VoiceState::Active)) return;
        const uint8_t note = v.note[handle];
        const bool awaitingNoteOff =
            channelKeyVoiceHead_[channel][note] == static_cast<int32_t>(handle) ||
            v.prevChannelKeyVoice[handle] >= 0 ||
            v.nextChannelKeyVoice[handle] >= 0;
        if (awaitingNoteOff) v.heldBySostenuto[handle] = 1u;
    });
}

inline void VoiceManager::ReleaseSostenuto(uint8_t channel,
                                            uint32_t blockOffset) {
    if (channel >= kChannelCount) return;
    ForEachChannelActive(channel, [&](VoiceHandle handle) {
        if (!v.heldBySostenuto[handle]) return;
        const uint8_t note = v.note[handle];
        const bool awaitingNoteOff =
            channelKeyVoiceHead_[channel][note] == static_cast<int32_t>(handle) ||
            v.prevChannelKeyVoice[handle] >= 0 ||
            v.nextChannelKeyVoice[handle] >= 0;
        v.heldBySostenuto[handle] = 0u;
        if (!awaitingNoteOff && !v.heldBySustain[handle]) {
            v.releaseStartInBlock[handle] = blockOffset;
            StartRelease(handle);
        }
    });
}

inline void VoiceManager::ReleaseSustain(uint8_t channel,
                                         uint32_t blockOffset) {
    if (channel >= kChannelCount) return;
    ForEachChannelActive(channel, [&](VoiceHandle handle) {
        if (!v.heldBySustain[handle]) return;
        v.heldBySustain[handle] = 0u;
        if (!v.heldBySostenuto[handle]) {
            v.releaseStartInBlock[handle] = blockOffset;
            StartRelease(handle);
        }
    });
}

inline uint32_t VoiceManager::NoteOffOldestPlayIndices(
        uint8_t channel, uint8_t note, uint32_t count, bool sustain,
        uint32_t blockOffset) {
    // `count` is a number of note-offs; the return value is how many of them
    // found a unit to release. An ordinary generation is one unit. A merged
    // generation of m units takes up to m offs: fewer than m releases only
    // that many units, by forking them off into a releasing twin.
    uint32_t released = 0u;
    while (released < count) {
        const uint32_t playIndex = FindOldestPlayIndex(channel, note);
        if (playIndex == UINT32_MAX) break;
        const uint32_t units = GetVoiceMultiplicity(static_cast<VoiceHandle>(
            channelKeyVoiceOldest_[channel][note]));
        const uint32_t remaining = count - released;
        if (units > 1u && remaining < units) {
            if (ForkReleasedUnits(channel, note, playIndex, remaining, sustain,
                                  blockOffset)) {
                released += remaining;
                continue;
            }
            // No slot for the twin even after stealing: release the whole
            // generation rather than leave the key hung.
            ++forkFailureCount_;
        }
        NoteOffPlayIndex(channel, note, playIndex, sustain, blockOffset);
        released += units < remaining ? units : remaining;
    }
    return released;
}

// Splits `units` of the oldest generation on (channel, note) off into a
// twin that takes the note-off, leaving the rest sounding. Every layer of the
// generation is cloned, so a stereo pair stays a pair.
//
// Exactness: before the split the m units are identical voices, so the twin
// (k units) and the source (m-k units) are two copies of one trajectory whose
// gains sum to the original; the twin then follows the release envelope from
// its current gain. Nothing is audible at the split frame.
inline bool VoiceManager::ForkReleasedUnits(
    uint8_t channel, uint8_t note, uint32_t playIndex, uint32_t units,
    bool sustain, uint32_t blockOffset) {
    constexpr uint32_t kMaxForkLayers = 16u;
    VoiceHandle source[kMaxForkLayers];
    uint32_t layers = 0u;
    // The oldest generation is the chain tail and its layers are adjacent.
    int32_t cursor = channelKeyVoiceOldest_[channel][note];
    while (cursor >= 0 && layers < kMaxForkLayers) {
        const VoiceHandle handle = static_cast<VoiceHandle>(cursor);
        if (v.playIndex[handle] != playIndex) break;
        source[layers++] = handle;
        cursor = v.prevChannelKeyVoice[handle];
    }
    if (layers == 0u) return false;
    if (cursor >= 0 &&
        v.playIndex[static_cast<uint32_t>(cursor)] == playIndex) {
        return false;  // more layers than a fork tracks
    }
    // The twin is a dense copy of the source; a staged source would copy
    // stale arrays (and the share scaling below writes mix gains).
    for (uint32_t layer = 0u; layer < layers; ++layer)
        EnsureMaterialized(source[layer]);
    const uint32_t mult = GetVoiceMultiplicity(source[0]);
    if (units == 0u || units >= mult) return false;
    if (v.rot != nullptr) return false;  // twins carry no rotation state

    // A full pool makes AllocateVoiceOrSteal pick a victim, which could be
    // the very voice being split. Make the sources unattractive for the
    // duration of the allocation (steal scores are level-based).
    const bool mayHitSource = freeTop_ < layers ||
        activeCount_ + layers > voiceLimit_;
    float savedOutputGain[kMaxForkLayers];
    if (mayHitSource) {
        for (uint32_t i = 0u; i < layers; ++i) {
            savedOutputGain[i] = v.stealOutputGain[source[i]];
            v.stealOutputGain[source[i]] = 1.0e9f;
            UpdateStealCandidate(source[i]);
        }
    }
    VoiceHandle twin[kMaxForkLayers];
    uint32_t made = 0u;
    bool allocated = true;
    for (; made < layers; ++made) {
        twin[made] = AllocateVoiceOrSteal(
            channel, note, v.velocity[source[made]], nullptr, true, false,
            kInvalidVoice, 0u, kInvalidVoice);
        if (twin[made] == kInvalidVoice) {
            allocated = false;
            break;
        }
    }
    if (mayHitSource) {
        for (uint32_t i = 0u; i < layers; ++i) {
            v.stealOutputGain[source[i]] = savedOutputGain[i];
            UpdateStealCandidate(source[i]);
        }
    }
    if (!allocated) {
        for (uint32_t rollback = 0u; rollback < made; ++rollback)
            RetireVoice(twin[rollback]);
        return false;
    }

    uint32_t twinPlayIndex = nextForkPlayIndex_++;
    if (nextForkPlayIndex_ == UINT32_MAX) nextForkPlayIndex_ = 0x80000000u;
    mergedVoicesMayExist_ = true;
    const float sourceShare = static_cast<float>(mult - units) /
                              static_cast<float>(mult);
    const float twinShare = static_cast<float>(units) /
                            static_cast<float>(mult);
    for (uint32_t i = 0u; i < layers; ++i) {
        const VoiceHandle s = source[i];
        const VoiceHandle t = twin[i];
        // Keep what the allocation linked for the twin; take everything else
        // from the source row.
        const int32_t keyNext = v.nextChannelKeyVoice[t];
        const int32_t keyPrev = v.prevChannelKeyVoice[t];
        VoiceSoA::CopyVoice(v, t, v, s);
        v.nextChannelKeyVoice[t] = keyNext;
        v.prevChannelKeyVoice[t] = keyPrev;
        v.state[t] = static_cast<uint8_t>(VoiceState::Active);
        v.renderClass[t] = static_cast<uint8_t>(VoiceRenderClass::Generic);
        v.heldBySustain[t] = 0u;
        v.releaseStartInBlock[t] = 0u;
        exclusiveClass_[t] = exclusiveClass_[s];
        SetVoicePlayIndex(t, twinPlayIndex);
        v.mult[t] = units;
        v.mult[s] = mult - units;
        const auto scale = [this](VoiceHandle h, float share) {
            v.gainLeft[h] *= share;
            v.gainRight[h] *= share;
            v.mixGainL[h] *= share;
            v.mixGainR[h] *= share;
            v.renderGainL[h] *= share;
            v.renderGainR[h] *= share;
            v.stealOutputGain[h] *= share;
        };
        scale(t, twinShare);
        scale(s, sourceShare);
        LinkExclusiveClass(t);
        RefreshRenderClass(t);
    }
    CommitVoiceGroupConfigurations(twin, layers, false);
    for (uint32_t i = 0u; i < layers; ++i) {
        UpdateStealCandidate(source[i]);
        if (voiceForkedHook_)
            voiceForkedHook_(twin[i], source[i], voiceForkedUserData_);
    }
    // The twin now plays exactly like the units that were released: the
    // ordinary note-off logic handles pedal and sostenuto holds as well.
    NoteOffPlayIndex(channel, note, twinPlayIndex, sustain, blockOffset);
    return true;
}

inline uint32_t VoiceManager::ReExciteKey(uint8_t channel, uint8_t note,
                                          uint32_t attackFrames) noexcept {
    if (attackFrames == 0u) attackFrames = ReExciteAttackFrames();
    uint32_t found = 0u;
    ForEachChannelKeyVoice(channel, note, [&](VoiceHandle h) {
        if (v.state[h] != static_cast<uint8_t>(VoiceState::Active)) return;
        ++found;
        if (reExciteHook_ != nullptr) {
            reExciteHook_(h, reExciteUserData_);
            return;
        }
        ExciteVoiceRow(v, h, attackFrames);
        RefreshRenderClass(h);
    });
    return found;
}

inline uint64_t VoiceManager::CountEffectiveVoices() noexcept {
    uint64_t total = 0u;
    bool anyMerged = false;
    for (uint32_t position = 0u; position < activeCount_; ++position) {
        const VoiceHandle handle = activeList_[position];
        const uint32_t mult = GetVoiceMultiplicity(handle);
        total += mult;
        anyMerged |= mult > 1u;
    }
    mergedVoicesMayExist_ = anyMerged;
    return total;
}
