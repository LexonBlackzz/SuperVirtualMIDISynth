// Test cases split out of V3Tests.cpp; included inside its anonymous namespace.

void TestOverloadTimelineRecovery() {
    Check(svms::RecoverRealtimeRenderFrame(4096, 4120, 512) == 4096,
          "small QPC jitter does not move the render timeline");
    Check(svms::RecoverRealtimeRenderFrame(4096, 5000, 512) == 5000,
          "missed callback frames fast-forward the render timeline");
    Check(svms::RecoverRealtimeRenderFrame(5000, 4096, 512) == 5000,
          "wall-clock recovery never moves the render timeline backward");
    Check(!svms::IsObsoleteNoteOn(3000, 4096, 2048),
          "a note-on within one buffer is clamped instead of skipped");
    Check(svms::IsObsoleteNoteOn(1000, 4096, 2048),
          "a note-on from skipped audio time is discarded");
}

void TestExpressionAgeRetirementAndLoopWrap() {
    svms::ChannelCache channels;
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(8, 44100);
    voices->SetCurrentFrame(1000);
    const svms::VoiceHandle first = voices->AllocateVoice(0, 60, 127);
    const svms::VoiceHandle middle = voices->AllocateVoice(0, 61, 127);
    const svms::VoiceHandle last = voices->AllocateVoice(0, 62, 127);
    voices->SetVoiceGain(first, 1.0f, 1.0f);
    voices->RefreshMixGain(first, channels.GetParams()[0]);
    const float fullExpressionGain = voices->v.mixGainL[first];
    channels.ControlChange(0, 11, 64);
    channels.RebuildCache(cfg, 44100.0f);
    voices->RefreshMixGain(first, channels.GetParams()[0]);
    Check(NearlyEqual(voices->v.mixGainL[first] / fullExpressionGain, 64.0f / 127.0f),
          "CC11 expression participates in active voice gain");

    voices->RetireVoice(middle);
    Check(voices->GetActiveCount() == 2 &&
          voices->activePosition_[last] < voices->GetActiveCount() &&
          voices->activeList_[voices->activePosition_[last]] == last,
          "inverse active positions make middle retirement O(1) and coherent");
    voices->SetCurrentFrame(1245);
    Check(voices->GetVoiceAge(first) == 245,
          "voice age is derived from absolute birth frame");

    int16_t samples[18]{};
    for (uint32_t i = 0; i < 16; ++i) samples[i] = 8192;
    voices->SetVoiceSample(first, 0, 10, 2, 6, 1, 12.25f, 1);
    svms::phasefx::StoreFloat(voices->v, first, 3.5f);
    voices->SetVoiceEnvelope(first, 1.0f, 1.0f, 0, 0, 0, 0,
                             0.0f, 1.0f, 0.999f);
    float left[1]{};
    float right[1]{};
    svms::RenderScalar renderer;
    renderer.RenderBlock(*voices, channels, samples, 16, left, right, 1, cfg,
                         nullptr, 0, true, 1245);
    Check(NearlyEqual(svms::phasefx::LoadFloat(voices->v, first), 3.75f),
          "loop wrapping preserves overshoot across multiple loop lengths");
}

struct DifferentialDispatchContext {
    svms::VoiceManager* voices;
    svms::ChannelCache* channels;
    const svms::RuntimeConfigSnapshot* cfg;
    uint32_t order[256]{};
    uint32_t orderCount = 0;
    uint32_t playIndex = 1000;
};

void DispatchDifferentialEvent(const svms::RenderEvent& event, uint32_t,
                               void* userData) {
    auto* context = static_cast<DifferentialDispatchContext*>(userData);
    if (context->orderCount < std::size(context->order))
        context->order[context->orderCount++] = event.ingressSequence;

    switch (event.type) {
        case svms::RenderEventType::NoteOn: {
            const svms::VoiceHandle voice = context->voices->AllocateVoiceOrSteal(
                event.channel, event.data1, event.data2);
            if (voice == svms::kInvalidVoice) break;
            context->voices->SetVoicePlayIndex(voice, context->playIndex++);
            context->voices->SetVoiceSample(voice, 0, 4096, 64, 4032, 1,
                                             0.75f + event.data1 * 0.005f, 1);
            context->voices->SetVoiceEnvelope(voice, 0.6f, 0.5f, 0, 0, 17, 53,
                                               0.6f / 17.0f, 0.99f, 0.9995f,
                                               400);
            context->voices->SetVoiceGain(voice, 0.02f, 0.02f);
            context->voices->RefreshMixGain(
                voice, context->channels->GetParams()[event.channel]);
            break;
        }
        case svms::RenderEventType::NoteOff:
            for (uint32_t i = 0; i < context->voices->activeCount_; ++i) {
                const uint32_t voice = context->voices->activeList_[i];
                if (context->voices->v.channel[voice] == event.channel &&
                    context->voices->v.note[voice] == event.data1) {
                    context->voices->StartRelease(voice);
                }
            }
            break;
        case svms::RenderEventType::ControlChange:
            context->channels->ControlChange(event.channel, event.data1, event.data2);
            context->channels->RebuildCache(*context->cfg, 44100.0f);
            context->voices->RefreshMixGains(context->channels->GetParams());
            break;
        case svms::RenderEventType::Reset:
            context->voices->Reset();
            context->channels->Reset();
            context->channels->RebuildCache(*context->cfg, 44100.0f);
            break;
        default:
            break;
    }
}

void ConfigureDifferentialSeed(svms::VoiceManager& voices,
                               const svms::ChannelCache& channels) {
    // Start full so randomized note-ons exercise exact stealing and the
    // independent steal-tail batch in every differential buffer-size case.
    voices.Initialize(48, 44100);
    for (uint32_t i = 0; i < 48; ++i) {
        const uint8_t channel = static_cast<uint8_t>(i & 3u);
        const uint8_t note = static_cast<uint8_t>(48u + i % 24u);
        const svms::VoiceHandle voice = voices.AllocateVoice(
            channel, note, static_cast<uint8_t>(48u + i));
        const bool loop = (i % 4u) != 1u;
        voices.SetVoiceSample(voice, 0, 4096, 64, 4032, loop ? 1u : 0u,
                              0.4f + static_cast<float>(i % 17u) * 0.11f, 1);
        svms::phasefx::StoreFloat(voices.v, voice, static_cast<float>((i * 37u) % 1700u) + 0.375f);
        if ((i % 4u) == 2u) {
            voices.SetVoiceEnvelope(voice, 0.8f, 0.35f, 0, 0, 101 + i, 0,
                                    0.8f / static_cast<float>(101 + i),
                                    1.0f, 0.9997f, 700 + i);
        } else if ((i % 4u) == 3u) {
            voices.SetVoiceEnvelope(voice, 0.8f, 0.35f, 0, 0, 0, 307 + i,
                                    0.0f, 0.997f, 0.9997f, 700 + i);
            voices.StartRelease(voice);
        } else {
            voices.SetVoiceEnvelope(voice, 0.8f, 0.7f, 0, 0, 0, 0,
                                    0.0f, 1.0f, 0.9997f, 700 + i);
        }
        voices.SetVoicePlayIndex(voice, i + 1u);
        voices.SetVoiceGain(voice, 0.02f, 0.02f);
        voices.RefreshMixGain(voice, channels.GetParams()[channel]);
    }
}

void TestSpanRendererDifferential() {
    static constexpr uint32_t bufferSizes[] = {16, 64, 257, 2048, 8192};
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096 + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>((0.4f * std::sin(static_cast<float>(i) * 0.031f) +
                      0.15f * std::cos(static_cast<float>(i) * 0.079f)) * 32767.0f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.velocityCurve = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;

    for (const uint32_t frames : bufferSizes) {
        svms::ChannelCache seedChannels;
        seedChannels.SetMasterVolume(1.0f);
        seedChannels.RebuildCache(cfg, 44100.0f);
        auto seedVoices = std::make_unique<svms::VoiceManager>();
        ConfigureDifferentialSeed(*seedVoices, seedChannels);
        auto referenceVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        auto spanVoices = std::make_unique<svms::VoiceManager>(*seedVoices);
        svms::ChannelCache referenceChannels = seedChannels;
        svms::ChannelCache spanChannels = seedChannels;

        std::vector<svms::RenderEvent> events;
        uint32_t random = 0x6d2b79f5u ^ frames;
        for (uint32_t sequence = 0; sequence < 48; ++sequence) {
            random = random * 1664525u + 1013904223u;
            svms::RenderEvent event{};
            event.frameOffset = random % frames;
            event.ingressSequence = sequence;
            event.channel = static_cast<uint8_t>((random >> 8) & 3u);
            if ((sequence % 3u) == 0u) {
                event.type = svms::RenderEventType::NoteOn;
                event.data1 = static_cast<uint8_t>(72u + sequence % 24u);
                event.data2 = static_cast<uint8_t>(80u + sequence % 40u);
            } else if ((sequence % 3u) == 1u) {
                event.type = svms::RenderEventType::NoteOff;
                event.data1 = static_cast<uint8_t>(48u + sequence % 24u);
            } else {
                event.type = svms::RenderEventType::ControlChange;
                event.data1 = static_cast<uint8_t>((sequence & 1u) ? 11u : 7u);
                event.data2 = static_cast<uint8_t>(40u + sequence);
            }
            events.push_back(event);
        }
        std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
            if (a.frameOffset != b.frameOffset) return a.frameOffset < b.frameOffset;
            return a.ingressSequence < b.ingressSequence;
        });

        std::vector<float> referenceLeft(frames, 0.0f), referenceRight(frames, 0.0f);
        std::vector<float> spanLeft(frames, 0.0f), spanRight(frames, 0.0f);
        DifferentialDispatchContext referenceContext{
            referenceVoices.get(), &referenceChannels, &cfg};
        DifferentialDispatchContext spanContext{spanVoices.get(), &spanChannels, &cfg};
        auto referenceRenderer = std::make_unique<svms::RenderScalar>();
        auto spanRenderer = std::make_unique<svms::RenderScalar>();
        referenceRenderer->SetEventDispatcher(DispatchDifferentialEvent, &referenceContext);
        spanRenderer->SetEventDispatcher(DispatchDifferentialEvent, &spanContext);

        referenceRenderer->RenderBlockReference(
            *referenceVoices, referenceChannels, samples.data(),
            sampleCount, referenceLeft.data(),
            referenceRight.data(), frames, cfg, events.data(),
            static_cast<uint32_t>(events.size()), true, 10000);
        spanRenderer->RenderBlock(
            *spanVoices, spanChannels, samples.data(),
            sampleCount, spanLeft.data(), spanRight.data(),
            frames, cfg, events.data(), static_cast<uint32_t>(events.size()), true, 10000);

        Check(referenceContext.orderCount == spanContext.orderCount &&
                  std::memcmp(referenceContext.order, spanContext.order,
                              referenceContext.orderCount * sizeof(uint32_t)) == 0,
              "span renderer preserves exact event dispatch order");

        float maximumDifference = 0.0f;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            maximumDifference = (std::max)(maximumDifference,
                std::fabs(referenceLeft[frame] - spanLeft[frame]));
            maximumDifference = (std::max)(maximumDifference,
                std::fabs(referenceRight[frame] - spanRight[frame]));
        }
        Check(maximumDifference <= 2.0e-4f,
              "span waveform matches the frame-major scalar reference");

        std::vector<uint32_t> referenceActive(
            referenceVoices->activeList_,
            referenceVoices->activeList_ + referenceVoices->activeCount_);
        std::vector<uint32_t> spanActive(
            spanVoices->activeList_, spanVoices->activeList_ + spanVoices->activeCount_);
        std::sort(referenceActive.begin(), referenceActive.end());
        std::sort(spanActive.begin(), spanActive.end());
        if (referenceActive != spanActive) {
            std::fprintf(stderr, "differential active mismatch at %u frames: ref=%zu span=%zu\n",
                         frames, referenceActive.size(), spanActive.size());
        }
        Check(referenceActive == spanActive,
              "span renderer preserves active voice identity and retirement");
        for (uint32_t voice : referenceActive) {
            const bool stateMatches =
                referenceVoices->v.state[voice] == spanVoices->v.state[voice] &&
                NearlyEqual(svms::phasefx::LoadFloat(referenceVoices->v, voice),
                            svms::phasefx::LoadFloat(spanVoices->v, voice), 2.0e-3f) &&
                NearlyEqual(referenceVoices->v.currentGain[voice],
                            spanVoices->v.currentGain[voice], 2.0e-5f) &&
                referenceVoices->v.releaseSamplesRemaining[voice] ==
                    spanVoices->v.releaseSamplesRemaining[voice];
            if (!stateMatches) {
                std::fprintf(stderr,
                    "differential state mismatch frames=%u voice=%u state=%u/%u phase=%g/%g gain=%g/%g release=%u/%u\n",
                    frames, voice, referenceVoices->v.state[voice], spanVoices->v.state[voice],
                    svms::phasefx::LoadFloat(referenceVoices->v, voice), svms::phasefx::LoadFloat(spanVoices->v, voice),
                    referenceVoices->v.currentGain[voice], spanVoices->v.currentGain[voice],
                    referenceVoices->v.releaseSamplesRemaining[voice],
                    spanVoices->v.releaseSamplesRemaining[voice]);
            }
            Check(stateMatches, "span renderer preserves active voice state");
        }
    }
}


// ── Hilbert-pair rotation (form 2) ───────────────────────────────────────
// The exact analytic companion x̂ is built by the production builder from
// the sample store; modes 1/2/4 rotate with y = x·cosθ − x̂·sinθ.  The
// frame-major oracle and the production span renderer must agree
// sample-for-sample, and the seeded states must take form 2 (modes 1/2/4
// always rotate with the exact pair; there is no approximate fallback).
void TestHilbertPairDifferential() {
    const uint32_t sampleCount = 4096u;
    // Integer cycle counts so the signal is exactly periodic over the
    // (power-of-two) slice: the FFT-based pair is then the EXACT analytic
    // twin everywhere, edges included, and the only error left is i16
    // quantization.
    const float omega1 = 6.2831853f * 20.0f / static_cast<float>(sampleCount);
    const float omega2 = 6.2831853f * 51.0f / static_cast<float>(sampleCount);
    std::vector<int16_t> samples(4096 + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>((0.4f * std::sin(static_cast<float>(i) * omega1) +
                      0.15f * std::cos(static_cast<float>(i) * omega2)) * 32767.0f);

    // The whole slice is played as one loop, so its companion is the
    // periodic transform: the exact analytic twin everywhere, edges included.
    std::vector<int16_t> hilbert(samples.size(), 0);
    svms::BuildHilbertSlice(samples.data(), hilbert.data(),
                            {0u, sampleCount, 0u, sampleCount});

    // Builder sanity: for x = a·sin + b·cos the analytic twin is
    // x̂ = −a·cos + b·sin.
    for (uint32_t i = 0u; i < sampleCount; ++i) {
        const float expected = -0.4f * std::cos(static_cast<float>(i) * omega1) +
                               0.15f * std::sin(static_cast<float>(i) * omega2);
        const float built = static_cast<float>(hilbert[i]) * (1.0f / 32768.0f);
        Check(NearlyEqual(expected, built, 2.0e-4f),
              "periodic companion is the analytic twin");
    }

    // The waveform storm and the steal-free differential that used to live
    // here drove a raw SetVoice* dispatcher through the span renderer, which
    // was the only path that took rotation.  Rotation now rides the
    // whole-voice renderer, and TestWholeVoiceRotation{Steal,Launch}
    // Differential cover modes 1/2/4 (steals, launches, ghost tails) against
    // the frame-major oracle at ~1e-8.

    // Without a companion store there is nothing exact to rotate with: the
    // voice stays unrotated instead of falling back to an approximation.
    svms::VoiceRotationState unpaired{};
    svms::SeedVoiceRotation(unpaired, 1u, 12345u, 44100.0f);
    Check(unpaired.form == 2u, "analytic mode always seeds the exact-pair form");
    Check(svms::RotateVoiceSample(unpaired, 0.37f, nullptr, 0u, 1u, 0.5f) == 0.37f,
          "without a companion store the voice stays unrotated");
}

// Loop-aware analytic companion. A rotated voice is a pure phase shift only
// when x-hat is the Hilbert transform of what the voice actually plays: over
// a loop that is the PERIODIC transform of one loop period. The old builder
// transformed each sample slice as a whole, so x-hat stepped at every loop
// wrap and a rotated sustained voice clicked at the loop rate (buzz) while
// its level depended on the angle.
void TestHilbertPairLoopAware() {
    const double twoPi = 6.283185307179586;
    // Odd (prime) loop length; the even case is checked right below.
    const uint32_t attack = 1500u, loopLength = 347u, tail = 600u;
    const uint32_t loopStart = attack, loopEnd = attack + loopLength;
    const uint32_t frames = loopEnd + tail;
    auto loopWave = [&](uint32_t n) {
        const double p = twoPi * static_cast<double>(n % loopLength) / loopLength;
        return 0.35 * std::sin(3.0 * p) + 0.2 * std::cos(10.0 * p);
    };
    auto loopTwin = [&](uint32_t n) {
        const double p = twoPi * static_cast<double>(n % loopLength) / loopLength;
        return -0.35 * std::cos(3.0 * p) + 0.2 * std::sin(10.0 * p);
    };
    std::vector<int16_t> samples(frames + 8u, 0);
    for (uint32_t i = 0; i < frames; ++i) {
        double x;
        if (i < loopStart)
            x = 0.25 * std::sin(i * 0.4113) + 0.15 * std::sin(i * 0.0731);
        else if (i < loopEnd)
            x = loopWave(i - loopStart);
        else
            x = loopWave(i - loopStart) * std::exp(-static_cast<double>(i - loopEnd) / 200.0);
        samples[i] = static_cast<int16_t>(std::lrint(x * 32767.0));
    }
    std::vector<int16_t> companion(samples.size(), 0);
    svms::BuildHilbertSlice(samples.data(), companion.data(),
                            {0u, frames, loopStart, loopEnd});
    auto at = [&](uint32_t i) { return companion[i] / 32768.0; };

    double loopError = 0.0;
    for (uint32_t n = 0; n < loopLength; ++n)
        loopError = (std::max)(loopError, std::fabs(at(loopStart + n) - loopTwin(n)));
    Check(loopError <= 2.0e-4, "loop companion is the exact periodic analytic twin");
    Check(std::fabs(at(loopEnd) - at(loopStart)) <= 2.0e-4,
          "loop companion wraps seamlessly at the loop end");
    Check(std::fabs(at(loopStart - 1u) - loopTwin(loopLength - 1u)) <= 5.0e-4,
          "attack companion meets the loop companion at loop start");

    // Even loop length (the periodic kernel drops the Nyquist bin).
    {
        const uint32_t evenLength = 348u;
        std::vector<int16_t> evenLoop(evenLength + 8u, 0), evenHat(evenLoop.size(), 0);
        auto phase = [&](uint32_t n) { return twoPi * n / evenLength; };
        for (uint32_t n = 0; n < evenLength; ++n)
            evenLoop[n] = static_cast<int16_t>(std::lrint(
                (0.3 * std::sin(5.0 * phase(n)) + 0.2 * std::cos(17.0 * phase(n))) * 32767.0));
        svms::BuildHilbertSlice(evenLoop.data(), evenHat.data(),
                                {0u, evenLength, 0u, evenLength});
        double evenError = 0.0;
        for (uint32_t n = 0; n < evenLength; ++n) {
            const double twin = -0.3 * std::cos(5.0 * phase(n)) + 0.2 * std::sin(17.0 * phase(n));
            evenError = (std::max)(evenError, std::fabs(evenHat[n] / 32768.0 - twin));
        }
        Check(evenError <= 2.0e-4, "even-length loop companion is the exact analytic twin");
    }

    // One-shot: a burst at the very end of a power-of-two slice must not
    // leak into the slice start (a plain circular FFT puts them side by side).
    std::vector<int16_t> burst(4096u + 8u, 0), burstHat(burst.size(), 0);
    for (uint32_t i = 4032u; i < 4096u; ++i)
        burst[i] = static_cast<int16_t>(0.8 * 32767.0 * std::sin((i - 4032u) * 0.39));
    svms::BuildHilbertSlice(burst.data(), burstHat.data(), {0u, 4096u, 0u, 0u});
    double headLeak = 0.0;
    for (uint32_t i = 0; i < 16u; ++i)
        headLeak = (std::max)(headLeak, std::fabs(burstHat[i] / 32768.0));
    Check(headLeak <= 1.0e-3,
          "one-shot companion does not wrap the slice end into its start");

    // Render-level, production span path: a rotated sustained looping voice
    // keeps the coherent voice's energy at every angle and has no step at
    // the loop wraps.
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.velocityCurve = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    const uint32_t renderFrames = loopStart + 24u * loopLength;
    const uint32_t windowStart = renderFrames - 8u * loopLength;
    auto render = [&](uint32_t mode, float theta) {
        svms::VoiceManager voices;
        voices.Initialize(4, 44100);
        const svms::VoiceHandle h = voices.AllocateVoice(0, 60, 100);
        voices.SetVoiceSample(h, 0, frames, loopStart, loopEnd, 1u, 1.0f, 1);
        voices.SetVoiceEnvelope(h, 1.0f, 1.0f, 0, 0, 0, 0, 0.0f, 1.0f, 0.9997f);
        voices.SetVoiceGain(h, 0.5f, 0.5f);
        voices.RefreshMixGain(h, channels.GetParams()[0]);
        if (mode != 0u) {
            Check(voices.SetPhaseRotationMode(mode), "loop render seeds rotation");
            voices.v.rot[h].c = std::cos(theta);
            voices.v.rot[h].s = std::sin(theta);
        }
        svms::RenderScalar renderer;
        std::vector<float> left(renderFrames, 0.0f), right(renderFrames, 0.0f);
        for (uint32_t done = 0; done < renderFrames;) {
            const uint32_t n = (std::min)(1024u, renderFrames - done);
            renderer.RenderBlock(voices, channels, samples.data(), companion.data(),
                                 frames, left.data() + done, right.data() + done,
                                 n, cfg, nullptr, 0u, true, 10000);
            done += n;
        }
        return left;
    };
    auto energy = [&](const std::vector<float>& signal) {
        double sum = 0.0;
        for (uint32_t n = windowStart; n < renderFrames; ++n)
            sum += static_cast<double>(signal[n]) * signal[n];
        return sum;
    };
    const std::vector<float> coherent = render(0u, 0.0f);
    const double coherentEnergy = energy(coherent);
    Check(coherentEnergy > 1.0e-3, "coherent loop voice is audible");
    for (const float theta : {0.4f, 1.2f, 2.0f, 2.9f, 3.9f, 5.1f}) {
        const std::vector<float> rotated = render(1u, theta);
        const double ratio = energy(rotated) / coherentEnergy;
        if (std::fabs(ratio - 1.0) > 1.0e-3)
            std::fprintf(stderr, "loop rotation energy ratio %.6f at theta %.2f\n", ratio, theta);
        Check(std::fabs(ratio - 1.0) <= 1.0e-3,
              "rotated looping voice keeps its energy at every angle");
        float wrapStep = 0.0f, innerStep = 0.0f;
        for (uint32_t n = windowStart; n < renderFrames; ++n) {
            const float step = std::fabs(rotated[n] - rotated[n - 1u]);
            if ((n - loopStart) % loopLength == 0u)
                wrapStep = (std::max)(wrapStep, step);
            else
                innerStep = (std::max)(innerStep, step);
        }
        Check(wrapStep <= innerStep * 1.05f,
              "rotated looping voice has no step at the loop wrap");
    }

    // Mode 4 (Random) is the exact pair too; it differs from Sweep by a
    // per-voice sweep rate and direction.
    bool sawUp = false, sawDown = false;
    float slowest = 1.0e9f, fastest = 0.0f;
    for (uint64_t seed = 1u; seed <= 64u; ++seed) {
        svms::VoiceRotationState st{};
        svms::SeedVoiceRotation(st, 4u, seed * 7919u, 44100.0f);
        Check(st.form == 2u, "random mode seeds the exact-pair form");
        const float hz = std::atan2(st.ds, st.dc) * 44100.0f / 6.2831853f;
        sawUp = sawUp || hz > 0.0f;
        sawDown = sawDown || hz < 0.0f;
        slowest = (std::min)(slowest, std::fabs(hz));
        fastest = (std::max)(fastest, std::fabs(hz));
    }
    Check(sawUp && sawDown && slowest >= 0.124f && fastest <= 0.376f &&
              fastest - slowest > 0.1f,
          "random mode sweeps each voice at its own rate and direction");
    svms::VoiceRotationState sweep{};
    svms::SeedVoiceRotation(sweep, 2u, 99u, 44100.0f);
    Check(sweep.form == 2u &&
              std::fabs(std::atan2(sweep.ds, sweep.dc) * 44100.0f / 6.2831853f - 0.25f) < 1.0e-4f,
          "sweep mode keeps its fixed 0.25 Hz sweep");
}

void TestRenderBackendSelectionAndDenseEquivalence() {
    constexpr uint32_t voiceCount = 32u;
    constexpr uint32_t frames = 4u;
    const uint32_t sampleCount = 2048u;
    std::vector<int16_t> samples(2048 + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(std::sin(static_cast<float>(i) * 0.021f) * 32767.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    auto seed = std::make_unique<svms::VoiceManager>();
    seed->Initialize(voiceCount, 44100);
    for (uint32_t i = 0; i < voiceCount; ++i) {
        const svms::VoiceHandle h = seed->AllocateVoice(0, 60, 100);
        seed->SetVoiceSample(h, 0, 2048, 16, 2032, 1,
                             0.5f + static_cast<float>(i) * 0.01f, 1);
        seed->SetVoiceEnvelope(h, 1.0f, 1.0f, 0, 0, 0, 0,
                               0.0f, 1.0f, 0.9999f);
        seed->SetVoiceGain(h, 0.01f, 0.01f);
        seed->RefreshMixGain(h, channels.GetParams()[0]);
    }

    const svms::RenderBackend backends[] = {
        svms::RenderBackend::SSE2, svms::RenderBackend::AVX2};
    for (svms::RenderBackend backend : backends) {
        if (!svms::IsRenderBackendSupported(backend)) continue;
        auto scalarVoices = std::make_unique<svms::VoiceManager>(*seed);
        auto acceleratedVoices = std::make_unique<svms::VoiceManager>(*seed);
        svms::RenderScalar scalar;
        svms::RenderScalar accelerated;
        Check(scalar.SetRenderBackend(svms::RenderBackend::Scalar) &&
                  accelerated.SetRenderBackend(backend),
              "supported render backend can be selected explicitly");
        float scalarLeft[frames]{}, scalarRight[frames]{};
        float acceleratedLeft[frames]{}, acceleratedRight[frames]{};
        scalar.RenderBlock(*scalarVoices, channels, samples.data(),
            sampleCount, scalarLeft, scalarRight,
            frames, cfg, nullptr, 0, true, 1000u);
        accelerated.RenderBlock(*acceleratedVoices, channels, samples.data(),
            sampleCount, acceleratedLeft,
            acceleratedRight, frames, cfg, nullptr, 0, true, 1000u);
        for (uint32_t frame = 0; frame < frames; ++frame) {
            Check(NearlyEqual(scalarLeft[frame], acceleratedLeft[frame], 2.0e-5f) &&
                      NearlyEqual(scalarRight[frame], acceleratedRight[frame], 2.0e-5f),
                  "accelerated dense kernel matches scalar audio");
        }
        for (uint32_t h = 0; h < voiceCount; ++h)
            Check(NearlyEqual(svms::phasefx::LoadFloat(scalarVoices->v, h),
                              svms::phasefx::LoadFloat(acceleratedVoices->v, h), 1.0e-5f),
                  "accelerated dense kernel preserves phase state");
    }
}

void TestTransientClassKernelDifferential() {
    constexpr uint32_t voiceCount = 32u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096 + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(std::sin(static_cast<float>(i) * 0.017f) * 32767.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    struct Scenario {
        uint32_t attack;
        uint32_t decay;
        float phase;
        float phaseStep;
    };
    const Scenario scenarios[] = {
        {64u, 64u, 20.25f, 0.75f},
        {0u, 64u, 20.25f, 0.75f},
        {2u, 2u, 20.25f, 0.75f},
        {64u, 64u, 2030.75f, 3.25f},
    };

    for (uint32_t frames = 1u; frames <= 4u; ++frames) {
        for (const Scenario& scenario : scenarios) {
            auto seed = std::make_unique<svms::VoiceManager>();
            seed->Initialize(voiceCount, 44100);
            for (uint32_t i = 0; i < voiceCount; ++i) {
                const svms::VoiceHandle h = seed->AllocateVoice(0, 60, 100);
                seed->SetVoiceSample(h, 0, 4096, 16, 2032, 1,
                                     scenario.phaseStep, 1);
                seed->SetVoiceEnvelope(h, 1.0f, 0.4f, 0, 0,
                    scenario.attack, scenario.decay,
                    scenario.attack != 0u
                        ? 1.0f / static_cast<float>(scenario.attack)
                        : 0.0f,
                    0.97f, 0.9999f);
                seed->SetVoiceGain(h, 0.01f, 0.012f);
                seed->RefreshMixGain(h, channels.GetParams()[0]);
                svms::phasefx::StoreFloat(seed->v, h, scenario.phase +
                    static_cast<float>(i & 3u) * 0.03125f);
            }

            auto referenceVoices = std::make_unique<svms::VoiceManager>(*seed);
            auto spanVoices = std::make_unique<svms::VoiceManager>(*seed);
            svms::RenderScalar reference;
            svms::RenderScalar span;
            span.SetRenderBackend(svms::RenderBackend::Scalar);
            float referenceLeft[4]{}, referenceRight[4]{};
            float spanLeft[4]{}, spanRight[4]{};
            reference.RenderBlockReference(*referenceVoices, channels,
                samples.data(), sampleCount,
                referenceLeft, referenceRight, frames, cfg, nullptr, 0, true,
                4000u);
            span.RenderBlock(*spanVoices, channels, samples.data(),
                sampleCount, spanLeft, spanRight,
                frames, cfg, nullptr, 0, true, 4000u);

            for (uint32_t frame = 0; frame < frames; ++frame) {
                Check(NearlyEqual(referenceLeft[frame], spanLeft[frame],
                                  2.0e-5f) &&
                          NearlyEqual(referenceRight[frame], spanRight[frame],
                                      2.0e-5f),
                      "short transient class kernel matches oracle audio");
            }
            for (uint32_t h = 0; h < voiceCount; ++h) {
                Check(NearlyEqual(svms::phasefx::LoadFloat(referenceVoices->v, h),
                                  svms::phasefx::LoadFloat(spanVoices->v, h), 1.0e-5f) &&
                          NearlyEqual(referenceVoices->v.currentGain[h],
                                      spanVoices->v.currentGain[h], 1.0e-6f) &&
                          referenceVoices->v.envelopeStage[h] ==
                              spanVoices->v.envelopeStage[h] &&
                          referenceVoices->v.attackSamplesRemaining[h] ==
                              spanVoices->v.attackSamplesRemaining[h] &&
                          referenceVoices->v.decaySamplesRemaining[h] ==
                              spanVoices->v.decaySamplesRemaining[h],
                      "short transient class kernel preserves envelope state");
            }
            for (uint32_t classIndex = 0;
                 classIndex < svms::kVoiceRenderClassCount; ++classIndex) {
                const auto renderClass =
                    static_cast<svms::VoiceRenderClass>(classIndex);
                Check(referenceVoices->GetRenderClassCount(renderClass) ==
                          spanVoices->GetRenderClassCount(renderClass),
                      "short transient class kernel preserves class transitions");
            }
        }
    }
}

void TestTransientAVX2LongSpanDifferential() {
    // The AVX2 transient-loop kernel's 8-frame vector chunks only engage for
    // spans longer than 4 frames; this differential exercises them (including
    // mid-span stage transitions and loop wraps) against the per-sample
    // reference.
    constexpr uint32_t voiceCount = 32u;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096 + 8u, 0);
    for (uint32_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(std::sin(static_cast<float>(i) * 0.017f) * 32767.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    struct Scenario {
        uint32_t attack;
        uint32_t decay;
        float phase;
        float phaseStep;
    };
    const Scenario scenarios[] = {
        {64u, 64u, 20.25f, 0.75f},
        {0u, 64u, 20.25f, 0.75f},
        {2u, 2u, 20.25f, 0.75f},
        {64u, 64u, 2030.75f, 3.25f},
        {200u, 100u, 2028.5f, 0.4375f},
    };
    const uint32_t spanSizes[] = {7u, 8u, 9u, 64u, 257u};

    if (!svms::IsRenderBackendSupported(svms::RenderBackend::AVX2)) return;
    for (const uint32_t frames : spanSizes) {
        for (const Scenario& scenario : scenarios) {
            auto seed = std::make_unique<svms::VoiceManager>();
            seed->Initialize(voiceCount, 44100);
            for (uint32_t i = 0; i < voiceCount; ++i) {
                const svms::VoiceHandle h = seed->AllocateVoice(0, 60, 100);
                seed->SetVoiceSample(h, 0, 4096, 16, 2032, 1,
                                     scenario.phaseStep, 1);
                seed->SetVoiceEnvelope(h, 1.0f, 0.4f, 0, 0,
                    scenario.attack, scenario.decay,
                    scenario.attack != 0u
                        ? 1.0f / static_cast<float>(scenario.attack)
                        : 0.0f,
                    0.97f, 0.9999f);
                seed->SetVoiceGain(h, 0.01f, 0.012f);
                seed->RefreshMixGain(h, channels.GetParams()[0]);
                svms::phasefx::StoreFloat(seed->v, h, scenario.phase +
                    static_cast<float>(i & 3u) * 0.03125f);
            }

            auto referenceVoices = std::make_unique<svms::VoiceManager>(*seed);
            auto spanVoices = std::make_unique<svms::VoiceManager>(*seed);
            svms::RenderScalar reference;
            svms::RenderScalar accelerated;
            Check(accelerated.SetRenderBackend(svms::RenderBackend::AVX2),
                  "AVX2 backend selectable for transient long-span test");
            std::vector<float> referenceLeft(frames, 0.0f),
                referenceRight(frames, 0.0f);
            std::vector<float> spanLeft(frames, 0.0f),
                spanRight(frames, 0.0f);
            reference.RenderBlockReference(*referenceVoices, channels,
                samples.data(), sampleCount,
                referenceLeft.data(), referenceRight.data(), frames, cfg,
                nullptr, 0, true, 4000u);
            accelerated.RenderBlock(*spanVoices, channels, samples.data(),
                sampleCount, spanLeft.data(),
                spanRight.data(), frames, cfg, nullptr, 0, true, 4000u);

            for (uint32_t frame = 0; frame < frames; ++frame) {
                Check(NearlyEqual(referenceLeft[frame], spanLeft[frame],
                                  2.0e-5f) &&
                          NearlyEqual(referenceRight[frame], spanRight[frame],
                                      2.0e-5f),
                      "AVX2 transient long-span kernel matches oracle audio");
            }
            for (uint32_t h = 0; h < voiceCount; ++h) {
                Check(NearlyEqual(svms::phasefx::LoadFloat(referenceVoices->v, h),
                                  svms::phasefx::LoadFloat(spanVoices->v, h), 1.0e-5f) &&
                          NearlyEqual(referenceVoices->v.currentGain[h],
                                      spanVoices->v.currentGain[h], 1.0e-6f) &&
                          referenceVoices->v.envelopeStage[h] ==
                              spanVoices->v.envelopeStage[h] &&
                          referenceVoices->v.attackSamplesRemaining[h] ==
                              spanVoices->v.attackSamplesRemaining[h] &&
                          referenceVoices->v.decaySamplesRemaining[h] ==
                              spanVoices->v.decaySamplesRemaining[h],
                      "AVX2 transient long-span kernel preserves envelope state");
            }
            for (uint32_t classIndex = 0;
                 classIndex < svms::kVoiceRenderClassCount; ++classIndex) {
                const auto renderClass =
                    static_cast<svms::VoiceRenderClass>(classIndex);
                Check(referenceVoices->GetRenderClassCount(renderClass) ==
                          spanVoices->GetRenderClassCount(renderClass),
                      "AVX2 transient long-span kernel preserves class transitions");
            }
        }
    }
}



void TestRenderCallbackPurity() {
    constexpr uint32_t frames = 512;
    constexpr uint32_t voiceCount = 256;
    const uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096 + 8u, 8192);
    std::vector<float> left(frames, 0.0f), right(frames, 0.0f);
    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);
    auto voices = std::make_unique<svms::VoiceManager>();
    voices->Initialize(voiceCount, 44100);
    for (uint32_t i = 0; i < voiceCount; ++i) {
        const svms::VoiceHandle voice = voices->AllocateVoice(0, 60, 100);
        voices->SetVoiceSample(voice, 0, 4096, 64, 4032, 1, 1.0f, 1);
        voices->SetVoiceEnvelope(voice, 1.0f, 1.0f, 0, 0, 0, 0,
                                 0.0f, 1.0f, 0.999f);
        voices->SetVoiceGain(voice, 0.01f, 0.01f);
        voices->RefreshMixGain(voice, channels.GetParams()[0]);
    }
    auto renderer = std::make_unique<svms::RenderScalar>();

    g_realtimeAllocationCount.store(0, std::memory_order_relaxed);
    g_trackRealtimeAllocations = true;
    renderer->RenderBlock(*voices, channels, samples.data(),
                          sampleCount, left.data(),
                          right.data(), frames, cfg, nullptr, 0, true, 0);
    g_trackRealtimeAllocations = false;
    Check(g_realtimeAllocationCount.load(std::memory_order_relaxed) == 0,
          "production scalar render performs no heap allocation");
}

void TestParallelSustainedRenderDifferential() {
    constexpr uint32_t voiceCount = 1024u;
    constexpr uint32_t frames = 2048u;
    const uint32_t sampleCount = 8192u;
    std::vector<int16_t> samples(8192u + 8u, 0);
    for (uint32_t index = 0u; index < samples.size(); ++index)
        samples[index] = static_cast<int16_t>(0.4f * std::sin(static_cast<float>(index) * 0.037f) * 32767.0f);

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.interpolation = svms::InterpolationMode::Linear;
    cfg.correctnessMode = true;
    svms::ChannelCache channels;
    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    auto makeVoices = [&]() {
        auto result = std::make_unique<svms::VoiceManager>();
        Check(result->Initialize(voiceCount, 44100u),
              "parallel differential allocates voice storage");
        for (uint32_t index = 0u; index < voiceCount; ++index) {
            const svms::VoiceHandle voice = result->AllocateVoice(
                static_cast<uint8_t>(index & 15u),
                static_cast<uint8_t>(24u + index % 88u), 127u);
            result->SetVoiceSample(voice, 0u, 8192u, 64u, 8128u, 1u,
                0.5f + static_cast<float>(index % 31u) * 0.03125f, 1u);
            result->SetVoiceEnvelope(voice, 1.0f, 1.0f, 0u, 0u, 0u, 0u,
                                     0.0f, 1.0f, 0.999f);
            result->SetVoiceGain(voice, 0.0001f, 0.0001f);
            result->RefreshMixGain(voice,
                channels.GetParams()[index & 15u]);
        }
        return result;
    };

    auto serialVoices = makeVoices();
    auto parallelVoices = makeVoices();
    svms::RenderScalar serial;
    svms::RenderScalar parallel;
    Check(serial.ReserveVoiceCapacity(voiceCount) &&
              parallel.ReserveVoiceCapacity(voiceCount) &&
              serial.ConfigureRenderThreads(1u, frames) &&
              parallel.ConfigureRenderThreads(4u, frames) &&
              parallel.GetRenderThreadCount() == 4u,
          "parallel differential starts four deterministic render lanes");

    std::vector<float> serialLeft(frames, 0.0f), serialRight(frames, 0.0f);
    std::vector<float> parallelLeft(frames, 0.0f), parallelRight(frames, 0.0f);
    std::fprintf(stderr, "CKPT pre-serial\\n"); std::fflush(stderr);
    std::fprintf(stderr, "M3"); std::fflush(stderr); serial.RenderBlock(*serialVoices, channels, samples.data(),
        sampleCount, serialLeft.data(),
        serialRight.data(), frames, cfg, nullptr, 0u, true, 1000u);
    parallel.RenderBlock(*parallelVoices, channels, samples.data(),
        sampleCount, parallelLeft.data(),
        parallelRight.data(), frames, cfg, nullptr, 0u, true, 1000u);

    for (uint32_t frame = 0u; frame < frames; ++frame) {
        Check(NearlyEqual(serialLeft[frame], parallelLeft[frame], 5.0e-5f) &&
                  NearlyEqual(serialRight[frame], parallelRight[frame],
                              5.0e-5f),
              "parallel sustained renderer preserves waveform tolerance");
    }
    for (uint32_t voice = 0u; voice < voiceCount; ++voice) {
        Check(svms::phasefx::LoadFloat(serialVoices->v, voice) ==
                  svms::phasefx::LoadFloat(parallelVoices->v, voice) &&
                  serialVoices->v.state[voice] ==
                  parallelVoices->v.state[voice],
              "parallel sustained renderer preserves exact voice state");
    }
}

struct DensePlannerDispatchContext {
    svms::VoiceManager* voices = nullptr;
    const svms::ChannelCache* channels = nullptr;
    uint32_t playIndex = 10000u;
};

void DensePlannerDispatch(const svms::RenderEvent* events,
                          uint32_t eventCount, uint32_t,
                          void* userData) {
    auto& context = *static_cast<DensePlannerDispatchContext*>(userData);
    for (uint32_t index = 0u; index < eventCount; ++index) {
        const svms::RenderEvent& event = events[index];
        if (event.type != svms::RenderEventType::NoteOn) continue;
        svms::VoiceConfiguration setup{};
        setup.sampleStart = 0u;
        setup.sampleEnd = 128u;
        setup.loopStart = 16u;
        setup.loopEnd = 96u;
        setup.loopMode = 1u;
        setup.phaseStep = setup.basePhaseStep =
            0.637123f +
            static_cast<float>(event.data1 & 31u) * 0.030731f;
        setup.initialGain = setup.sustainLevel = 1.0f;
        setup.releaseDecay = 0.999f;
        setup.gainLeft = setup.gainRight = 0.001f;
        setup.sampleBacked = 1u;
        setup.playIndex = context.playIndex++;
        svms::VoiceHandle handle = svms::kInvalidVoice;
        context.voices->LaunchVoiceGroup(
            event.channel, event.data1, event.data2, &setup, 1u,
            setup.playIndex,
            context.channels->GetParams()[event.channel], &handle);
    }
}

void TestWholeVoiceProductionGateParity() {
    // The whole-voice whole-block renderer is the primary production
    // renderer for note material: it must produce audio within the oracle
    // tolerance with correctnessMode disabled (the live configuration) and
    // exercise pools above the historical 8192-voice cap of the removed dense planner.  Audio is
    // compared at the standard 2e-5 tolerance because per-voice whole-block
    // jobs accumulate in worker-count-dependent order (and the accelerated
    // backend is tolerance-equal, never bit-replacing, the scalar path).
    constexpr uint32_t voiceCount = 3000u;
    constexpr uint32_t frames = 256u;
    constexpr uint32_t sampleCount = 4096u;
    std::vector<int16_t> samples(4096u + 8u, 0);
    for (uint32_t index = 0u; index < samples.size(); ++index)
        samples[index] = static_cast<int16_t>(
            0.35f * std::sin(static_cast<float>(index) * 0.071f) * 32767.0f);

    svms::ChannelCache channels;
    auto makeVoices = [&]() {
        auto result = std::make_unique<svms::VoiceManager>();
        Check(result->Initialize(voiceCount, 44100u),
              "production gate parity allocates voice storage");
        for (uint32_t index = 0u; index < voiceCount; ++index) {
            const svms::VoiceHandle voice = result->AllocateVoice(
                static_cast<uint8_t>(index & 15u),
                static_cast<uint8_t>(24u + index % 88u), 127u);
            result->SetVoiceSample(voice, 0u, 128u, 16u, 96u, 1u,
                0.637123f + static_cast<float>(index & 31u) * 0.030731f, 1u);
            result->SetVoiceEnvelope(voice, 1.0f, 1.0f, 0u, 0u, 0u, 0u,
                                     0.0f, 1.0f, 0.999f);
            result->SetVoiceGain(voice, 0.001f, 0.001f);
            result->SetVoicePlayIndex(voice, index + 1u);
            result->RefreshMixGain(voice,
                channels.GetParams()[index & 15u]);
        }
        return result;
    };

    constexpr uint32_t eventsPerFrame = 8u;
    constexpr uint8_t notes[4] = {39u, 51u, 63u, 75u};
    std::vector<svms::RenderEvent> events(frames * eventsPerFrame);
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        for (uint32_t lane = 0u; lane < eventsPerFrame; ++lane) {
            const uint32_t index = frame * eventsPerFrame + lane;
            events[index].type = svms::RenderEventType::NoteOn;
            events[index].channel = static_cast<uint8_t>(lane & 15u);
            events[index].data1 = notes[lane & 3u];
            events[index].data2 = 127u;
            events[index].frameOffset = frame;
            events[index].ingressSequence = index;
        }
    }

    svms::RuntimeConfigSnapshot cfg{};
    cfg.masterVolume = 1.0f;
    cfg.panLaw = svms::PanLaw::ConstantPower;
    cfg.interpolation = svms::InterpolationMode::Linear;
    cfg.correctnessMode = false;

    channels.SetMasterVolume(1.0f);
    channels.RebuildCache(cfg, 44100.0f);

    auto productionVoices = makeVoices();
    auto referenceVoices = makeVoices();
    auto serialVoices = makeVoices();
    svms::RenderScalar production;
    svms::RenderScalar reference;
    svms::RenderScalar serial;
    Check(production.ReserveVoiceCapacity(voiceCount) &&
              reference.ReserveVoiceCapacity(voiceCount) &&
              production.ConfigureRenderThreads(6u, frames) &&
              reference.ConfigureRenderThreads(4u, frames) &&
              serial.ConfigureRenderThreads(1u, frames),
          "production gate parity starts parallel and serial renderers");
    DensePlannerDispatchContext productionContext{productionVoices.get(),
                                                   &channels};
    DensePlannerDispatchContext referenceContext{referenceVoices.get(),
                                                  &channels};
    DensePlannerDispatchContext serialContext{serialVoices.get(), &channels};
    production.SetEventBatchDispatcher(DensePlannerDispatch,
                                        &productionContext);
    reference.SetEventBatchDispatcher(DensePlannerDispatch, &referenceContext);
    serial.SetEventBatchDispatcher(DensePlannerDispatch, &serialContext);
    std::vector<float> productionLeft(frames, 0.0f),
        productionRight(frames, 0.0f);
    std::vector<float> referenceLeft(frames, 0.0f),
        referenceRight(frames, 0.0f);
    production.RenderBlock(*productionVoices, channels, samples.data(),
        sampleCount, productionLeft.data(), productionRight.data(), frames,
        cfg, events.data(), static_cast<uint32_t>(events.size()), true,
        5000u);
    reference.RenderBlock(*referenceVoices, channels, samples.data(),
        sampleCount, referenceLeft.data(), referenceRight.data(), frames,
        cfg, events.data(), static_cast<uint32_t>(events.size()), true,
        5000u);
    std::vector<float> serialLeft(frames, 0.0f), serialRight(frames, 0.0f);
    serial.RenderBlock(*serialVoices, channels, samples.data(),
        sampleCount, serialLeft.data(), serialRight.data(), frames,
        cfg, events.data(), static_cast<uint32_t>(events.size()), false,
        5000u);
    Check(production.GetWholeVoiceBlocksForTest() == 1u,
          "production configuration routes the callback through the whole-voice renderer");
    {
        float maxDiff = 0.0f; uint32_t firstFrame = UINT32_MAX;
        for (uint32_t frame = 0u; frame < frames; ++frame) {
            const float dl = std::fabs(productionLeft[frame] - referenceLeft[frame]);
            const float dr = std::fabs(productionRight[frame] - referenceRight[frame]);
            if (dl > maxDiff) maxDiff = dl;
            if (dr > maxDiff) maxDiff = dr;
            if ((dl != 0.0f || dr != 0.0f) && firstFrame == UINT32_MAX) firstFrame = frame;
        }
        fprintf(stderr, "PARITYDIFF max=%g first=%u\\n", maxDiff, firstFrame);
    }
    float maxParityDifference = 0.0f;
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        maxParityDifference = (std::max)(maxParityDifference,
            std::fabs(productionLeft[frame] - referenceLeft[frame]));
        maxParityDifference = (std::max)(maxParityDifference,
            std::fabs(productionRight[frame] - referenceRight[frame]));
    }
    Check(maxParityDifference <= 2.0e-5f,
          "whole-voice renderer matches the reference within oracle tolerance");
}

void TestCallbackSourcePurity() {
    const std::filesystem::path sourcePath =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "driver" / "Driver.cpp";
    std::ifstream input(sourcePath, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(input)), {});
    const size_t callbackBegin = source.find("void Driver::RenderCallback");
    const size_t callbackEnd = source.find("} // namespace svms", callbackBegin);
    Check(callbackBegin != std::string::npos && callbackEnd != std::string::npos,
          "callback purity audit locates the production real-time section");
    if (callbackBegin == std::string::npos || callbackEnd == std::string::npos) return;
    const std::string realtimeSection =
        source.substr(callbackBegin, callbackEnd - callbackBegin);
    Check(realtimeSection.find("EnterCriticalSection") == std::string::npos &&
              realtimeSection.find("OutputDebugString") == std::string::npos &&
              realtimeSection.find("LOG(") == std::string::npos &&
              realtimeSection.find("DiagWindow_Create") == std::string::npos &&
              realtimeSection.find("CreateWindow") == std::string::npos,
          "audio callback and MIDI dispatch contain no lock, debug, or UI calls");
}
