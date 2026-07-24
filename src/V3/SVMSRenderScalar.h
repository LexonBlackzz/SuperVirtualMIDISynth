#ifndef SVMS_RENDER_SCALAR_H
#define SVMS_RENDER_SCALAR_H

#include "SVMSTypes.h"
#include "SVMSVoiceManager.h"
#include "SVMSChannelCache.h"
#include "SVMSEnvelope.h"
#include "SVMSPageAllocator.h"
#include "SVMSSoundFont.h"

namespace svms {

// ── Linear interpolation between two sample frames ──────────────────────
inline float InterpolateSample(const float* data, uint32_t baseIndex,
                                uint32_t nextIndex, float frac) {
    const float s0 = data[baseIndex];
    const float s1 = data[nextIndex];
    return s0 + (s1 - s0) * frac;
}

// ── Loop eligibility check ──────────────────────────────────────────────
inline bool ShouldLoopSVMS(uint8_t loopMode, uint32_t loopStart, uint32_t loopEnd,
                            uint8_t state) {
    if (loopMode == 0) return false;
    if (state == static_cast<uint8_t>(VoiceState::Releasing) && loopMode == 2)
        return false;
    return loopEnd > loopStart + 1u;
}

// ════════════════════════════════════════════════════════════════════════
// Event descriptor for sub-sample-precise dispatch.
// `sampleOffset` is a float so it carries fractional-sample resolution
// (e.g. 127.37 = 127 samples + 0.37 of a sample into the block).
// ════════════════════════════════════════════════════════════════════════
enum class RenderEventType : uint8_t {
    NoteOn       = 0,
    NoteOff      = 1,
    ControlChange= 2,
    ProgramChange= 3,
    PitchBend    = 4,
    AllNotesOff  = 5,
    AllSoundOff  = 6,
};

struct RenderEvent {
    RenderEventType type;
    uint8_t  channel;
    uint8_t  data1;     // note or controller
    uint8_t  data2;     // velocity or value
    float    sampleOffset; // fractional sample position within the block
};

// ════════════════════════════════════════════════════════════════════════
// Event dispatcher callback.
//
// The Driver registers a function of this signature with RenderScalar.
// During RenderBlock, at each event's exact sampleOffset, the renderer
// calls this dispatcher so the Driver can perform voice allocation,
// release, CC updates, etc.  The dispatcher receives the event and the
// current blockCursor (absolute sample within the block) so it can set
// phaseOffset and releaseStartInBlock on newly allocated / released voices.
//
// Parameters:
//   event       — the RenderEvent to dispatch
//   blockCursor — the integer sample position within the block where the
//                 event fires (floor of event.sampleOffset)
//   userData    — opaque pointer to the Driver instance
// ════════════════════════════════════════════════════════════════════════
using EventDispatcher = void(*)(const RenderEvent& event, uint32_t blockCursor,
                                 void* userData);

// ════════════════════════════════════════════════════════════════════════
// RenderScalar — sub-sample-accurate scalar render pipeline.
//
// Architecture:
//   1. Caller (Driver) pushes timestamped MIDI events into an SPSC queue.
//      Before each RenderBlock call, the Driver drains the queue into a
//      sorted RenderEvent array with fractional sample offsets computed
//      from QPC timestamps.
//   2. RenderBlock walks the event list, slicing the render at each
//      event boundary.  Between consecutive events, every active voice
//      is processed for exactly that sub-block length.
//   3. At each event boundary, the registered EventDispatcher callback
//      is invoked so the Driver can handle voice allocation, release,
//      CC updates, etc.
//   4. Each newly triggered voice carries a `phaseOffset` that
//      initializes its sample reader at the exact fractional position,
//      enabling sub-sample event timing without quantization.
//
// This code path is identical for live WASAPI output and deterministic
// offline file rendering — the only difference is how the event array
// is populated.
// ════════════════════════════════════════════════════════════════════════
class RenderScalar {
public:
    RenderScalar();

    // ── Primary entry point ─────────────────────────────────────────────
    // Renders `numFrames` of audio into the planar L/R output buffers.
    // `events` / `eventCount` describe timed MIDI events within the block,
    // sorted by sampleOffset.  Pass eventCount=0 for event-free blocks.
    //
    // [HOOK] Density management / decimation: wrap this call with a layer
    //        that decides how many voices to actually process when voice
    //        pressure is extreme.  The core path is deterministic, so any
    //        voice culling must be audibly transparent.
    void RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                     const float* sampleData, uint32_t sampleDataFrames,
                     float* outputLeft, float* outputRight,
                     uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                     const RenderEvent* events = nullptr,
                     uint32_t eventCount = 0);

    // ── Dispatcher registration ─────────────────────────────────────────
    // The Driver calls this once during initialization to register its
    // event handler.  The renderer invokes the dispatcher at each event's
    // exact sample offset during RenderBlock.
    void SetEventDispatcher(EventDispatcher dispatcher, void* userData);

private:
    // Process a single voice for a contiguous sub-block [startFrame, startFrame+numFrames).
    // `startFrame` is the absolute sample position within the current block,
    // used for release gate timing.  The output pointers are already advanced
    // to the correct write position.
    //
    // On the first sub-block after a note-on, phaseOffset positions the
    // read cursor at the exact fractional trigger point.  After that first
    // sub-block, phaseOffset is zeroed so subsequent sub-blocks continue
    // from where the voice left off.
    //
    // [HOOK] Voice-density scaling: a future layer can wrap this to skip
    //        voices that are perceptually masked or below an audibility
    //        threshold under heavy polyphony.
    void ProcessVoice(VoiceManager& voices, VoiceHandle handle,
                      const ChannelParamsSnapshot& chanParams,
                      const float* sampleData, uint32_t sampleDataFrames,
                      float* outLeft, float* outRight,
                      uint32_t startFrame, uint32_t numFrames,
                      const RuntimeConfigSnapshot& cfg);

    EventDispatcher dispatcher_;
    void* dispatcherUserData_;
};

inline RenderScalar::RenderScalar()
    : dispatcher_(nullptr), dispatcherUserData_(nullptr) {}

inline void RenderScalar::SetEventDispatcher(EventDispatcher dispatcher, void* userData) {
    dispatcher_ = dispatcher;
    dispatcherUserData_ = userData;
}

// ════════════════════════════════════════════════════════════════════════
// ProcessVoice — render one voice for a sub-block of frames.
//
// The voice reads from the sample buffer starting at its current phase.
// On the first sub-block after a note-on, phaseOffset positions the
// read cursor at the exact fractional trigger point.  After that first
// sub-block, phaseOffset is zeroed so subsequent sub-blocks continue
// from where the voice left off.
//
// The envelope (delay/hold/attack/decay/sustain/release) is evaluated
// per-sample.  Release respects releaseStartInBlock so note-offs
// triggered mid-block take effect at the exact frame.
// ════════════════════════════════════════════════════════════════════════
inline void RenderScalar::ProcessVoice(VoiceManager& voices, VoiceHandle handle,
                                        const ChannelParamsSnapshot& chanParams,
                                        const float* sampleData, uint32_t sampleDataFrames,
                                        float* outLeft, float* outRight,
                                        uint32_t startFrame, uint32_t numFrames,
                                        const RuntimeConfigSnapshot& cfg) {
    (void)cfg;
    VoiceSoA& v = voices.v;

    // Apply phaseOffset on first sub-block after trigger, then clear it
    // so it does not re-accumulate on subsequent sub-blocks.
    float phase = v.phases[handle] + v.phaseOffset[handle];
    v.phaseOffset[handle] = 0.0f;

    float phaseStep = v.phaseIncs[handle];
    float gain = v.currentGain[handle];
    uint8_t voiceState = v.state[handle];
    float voiceGainL = v.gainLeft[handle];
    float voiceGainR = v.gainRight[handle];

    const uint32_t sStart = v.sampleStart[handle];
    const uint32_t sEnd = v.sampleEnd[handle];
    const uint32_t sLoopS = v.loopStart[handle];
    const uint32_t sLoopE = v.loopEnd[handle];
    const uint8_t  sLoopM = v.loopMode[handle];
    const uint8_t  sb = v.sampleBacked[handle];

    const bool isSampleBacked = (sb != 0 && sampleData != nullptr);
    const bool isReleased = (voiceState == static_cast<uint8_t>(VoiceState::Releasing));

    uint32_t relativeSampleEnd = 0;
    uint32_t relativeLoopStart = 0;
    uint32_t relativeLoopEnd = 0;
    if (isSampleBacked) {
        relativeSampleEnd = sEnd - sStart;
        relativeLoopStart = (sLoopS > sStart) ? sLoopS - sStart : 0u;
        relativeLoopEnd = (sLoopE > sStart) ? sLoopE - sStart : 0u;
    }
    const bool loop = isSampleBacked && ShouldLoopSVMS(sLoopM, sLoopS, sLoopE, voiceState);

    const float chanVol = chanParams.volume;
    const float panL = chanParams.panLeft;
    const float panR = chanParams.panRight;

    // Release gate is absolute (relative to block start).
    const uint32_t releaseGate = v.releaseStartInBlock[handle];
    bool retireVoice = false;

    for (uint32_t f = 0; f < numFrames; ++f) {
        float sample = 0.0f;

        if (isSampleBacked) {
            if (phase < 0.0f) phase = 0.0f;

            uint32_t baseOffset = static_cast<uint32_t>(phase);
            if (baseOffset + 1u >= relativeSampleEnd) {
                if (!loop) {
                    retireVoice = true;
                    break;
                }
                phase = static_cast<float>(relativeLoopStart);
                baseOffset = relativeLoopStart;
            }

            uint32_t baseIndex = sStart + baseOffset;
            uint32_t nextIndex = baseIndex + 1u;

            if (loop && nextIndex >= sLoopE)
                nextIndex = sLoopS;
            if (nextIndex >= sEnd)
                nextIndex = sEnd - 1u;

            const float frac = phase - static_cast<float>(baseOffset);
            sample = InterpolateSample(sampleData, baseIndex, nextIndex, frac);

            if (!isReleased) {
                // ── Envelope: delay → hold → attack → decay → sustain ──
                if (v.envelopeStage[handle] == 4) {
                    if (v.delaySamplesRemaining[handle] > 0) {
                        --v.delaySamplesRemaining[handle];
                        gain = 0.0f;
                    } else {
                        v.envelopeStage[handle] = 0;
                    }
                }

                if (v.envelopeStage[handle] == 0) {
                    if (v.holdSamplesRemaining[handle] > 0) {
                        --v.holdSamplesRemaining[handle];
                        gain = v.targetGain[handle];
                    } else {
                        v.envelopeStage[handle] = 1;
                    }
                }

                if (v.envelopeStage[handle] == 1) {
                    if (v.attackSamplesRemaining[handle] > 0) {
                        gain += v.attackGainStep[handle];
                        --v.attackSamplesRemaining[handle];
                        if (gain > v.targetGain[handle]) gain = v.targetGain[handle];
                    } else {
                        gain = v.targetGain[handle];
                    }
                    if (v.attackSamplesRemaining[handle] == 0) {
                        v.envelopeStage[handle] = (v.decaySamplesRemaining[handle] > 0) ? 2 : 3;
                    }
                }

                if (v.envelopeStage[handle] == 2) {
                    if (v.decaySamplesRemaining[handle] > 0) {
                        gain *= v.decaySlope[handle];
                        --v.decaySamplesRemaining[handle];
                        if (gain < v.sustainLevel[handle]) gain = v.sustainLevel[handle];
                    } else {
                        gain = v.sustainLevel[handle];
                    }
                    if (v.decaySamplesRemaining[handle] == 0) {
                        v.envelopeStage[handle] = 3;
                    }
                }
            } else {
                // Release: gain decays after the absolute frame reaches
                // the release trigger point (releaseStartInBlock).
                if ((startFrame + f) >= releaseGate) {
                    gain *= v.releaseDecay[handle];
                }
            }

            sample *= gain;
            phase += phaseStep;

            if (loop && phase >= static_cast<float>(relativeLoopEnd)) {
                phase = static_cast<float>(relativeLoopStart) +
                        (phase - static_cast<float>(relativeLoopEnd));
            }
        }

        float outL = sample * voiceGainL * panL * chanVol;
        float outR = sample * voiceGainR * panR * chanVol;
        outLeft[f] += outL;
        outRight[f] += outR;
    }

    v.phases[handle] = phase;
    v.currentGain[handle] = gain;

    if (retireVoice || (isReleased && gain < kVoiceRetireThreshold)) {
        voices.RetireVoice(handle);
    }
}

// ════════════════════════════════════════════════════════════════════════
// RenderBlock — sub-sample event-sliced render.
//
// The event array MUST be sorted by ascending sampleOffset.
// Events are dispatched at their exact fractional position:
//   1. All events whose sampleOffset falls at the current cursor are
//      dispatched via the EventDispatcher callback.
//   2. Active voices are rendered for the sub-block from cursor to the
//      next event boundary (or end of block).
//   3. The cursor advances and the process repeats.
//
// For zero-length sub-blocks (multiple events at the same sample),
// only dispatch occurs — no voice rendering is needed.
//
// This is the single code path used by both real-time WASAPI output and
// deterministic offline file rendering.
// ════════════════════════════════════════════════════════════════════════
inline void RenderScalar::RenderBlock(VoiceManager& voices, const ChannelCache& channels,
                                       const float* sampleData, uint32_t sampleDataFrames,
                                       float* outputLeft, float* outputRight,
                                       uint32_t numFrames, const RuntimeConfigSnapshot& cfg,
                                       const RenderEvent* events, uint32_t eventCount) {
    VoiceSoA& v = voices.v;
    uint32_t maxVoices = voices.GetMaxVoices();
    const ChannelParamsSnapshot* chParams = channels.GetParams();

    // ── [HOOK] Grouping layer: group voices by channel / key / instrument
    //    for efficient cache-friendly processing.  Insert before the
    //    per-voice loop.

    // ── [HOOK] Voice-density scaling: when activeCount > threshold,
    //    mark low-priority voices for decimation.  Insert here.

    uint32_t cursor = 0;
    uint32_t ei = 0;

    while (cursor < numFrames) {
        // ── Dispatch all events at or before the current cursor ────────
        // Events at the same fractional position are batched: all are
        // dispatched before any rendering occurs at that position.
        while (ei < eventCount) {
            float evtOffset = events[ei].sampleOffset;
            if (evtOffset >= static_cast<float>(cursor) + 1.0f)
                break; // next event is beyond this cursor

            // Event fires at this cursor position.
            if (dispatcher_) {
                // ── [HOOK] Event classification / grouping: the dispatcher
                //    can classify events into voice allocation, release, or
                //    CC update paths.  Current implementation dispatches
                //    directly to Driver handlers.
                dispatcher_(events[ei], cursor, dispatcherUserData_);
            }
            ++ei;
        }

        // ── Determine sub-block end ────────────────────────────────────
        // The sub-block extends from `cursor` to the next event's
        // sampleOffset, or to end-of-block if no more events.
        uint32_t subEnd = numFrames;
        if (ei < eventCount) {
            uint32_t nextEvt = static_cast<uint32_t>(events[ei].sampleOffset);
            if (nextEvt < subEnd) subEnd = nextEvt;
        }
        if (subEnd < cursor) subEnd = cursor;

        uint32_t subLen = subEnd - cursor;

        // For events at the exact same sample position, subLen can be 0.
        // In that case we skip rendering (dispatch-only) and advance the
        // cursor to re-evaluate on the next iteration.
        if (subLen == 0) {
            cursor = subEnd;
            continue;
        }

        // ── Render all active voices for this sub-block ────────────────
        // Output pointers are advanced by `cursor` so voices write into
        // the correct position within the full block.
        float* subOutL = outputLeft + cursor;
        float* subOutR = outputRight + cursor;

        for (uint32_t i = 0; i < maxVoices; ++i) {
            if (v.state[i] == static_cast<uint8_t>(VoiceState::Free)) continue;
            uint8_t ch = v.channel[i];
            ProcessVoice(voices, static_cast<VoiceHandle>(i), chParams[ch],
                         sampleData, sampleDataFrames,
                         subOutL, subOutR,
                         cursor, subLen, cfg);
        }

        cursor = subEnd;
    }

    // ── [HOOK] Decimation: if voice count exceeded budget, retroactively
    //    zero the output for decimated frames.  Insert after the render loop.
    //
    // ── [HOOK] Overload ladder: check active voice count against soft/
    //    hard/panic thresholds and flag RenderStats accordingly.
}

} // namespace svms

#endif
