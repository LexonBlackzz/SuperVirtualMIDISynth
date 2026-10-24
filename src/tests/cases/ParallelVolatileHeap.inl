// The volatile steal heap's key build can fan out over the worker pool (it is
// a cache-miss-bound pass over every releasing voice).  Both ways must pick
// the same victims in the same order.

void FakeReverseParallelFor(void* user, uint32_t jobCount,
                            void (*job)(void* context, uint32_t index),
                            void* context) {
    ++*static_cast<uint32_t*>(user);
    for (uint32_t i = jobCount; i-- > 0u;) job(context, i);
}

void TestParallelVolatileHeap() {
    constexpr uint32_t kPool = 70000u;
    constexpr uint32_t kSteals = 3000u;
    std::vector<svms::VoiceHandle> picked[2];
    uint32_t fanouts = 0u;
    for (uint32_t variant = 0u; variant < 2u; ++variant) {
        auto voices = std::make_unique<svms::VoiceManager>();
        Check(voices->Initialize(kPool, 44100u), "volatile heap pool allocates");
        voices->SetStealKeyBackend(svms::RenderBackend::AVX2);
        for (uint32_t i = 0u; i < kPool; ++i) {
            const svms::VoiceHandle h = voices->AllocateVoice(
                static_cast<uint8_t>(i & 15u), static_cast<uint8_t>(24u + i % 80u), 100u);
            voices->SetVoiceSample(h, 0, 4096, 64, 4032, 1u, 1.0f, 1);
            // Distinct, hash-shuffled levels so the victim order is unambiguous.
            const uint32_t hash = (i * 2654435761u) >> 8;
            // Decaying voices are volatile steal candidates (their level moves
            // every block) and are not served by the releasing ring.
            voices->SetVoiceEnvelope(h, 0.001f + static_cast<float>(hash & 0xFFFFu) * 1.0e-5f,
                                     0.3f, 0, 0, 0, 100000u, 0.0f, 0.9999f, 0.999f);
            voices->SetVoicePlayIndex(h, i + 1u);
            voices->v.mixGainL[h] = voices->v.mixGainR[h] = 0.5f;
            voices->v.stealOutputGain[h] = 0.7f;
        }
        if (variant == 1u)
            voices->SetParallelFor(&FakeReverseParallelFor, &fanouts);
        for (uint32_t s = 0u; s < kSteals; ++s) {
            bool stolen = false;
            const svms::VoiceHandle h =
                voices->AllocateVoiceOrSteal(0, 60, 100, &stolen);
            picked[variant].push_back(stolen ? h : svms::kInvalidVoice);
        }
    }
    Check(fanouts != 0u, "the volatile heap build used the parallel hook");
    Check(picked[0] == picked[1],
          "parallel and serial volatile key builds pick the same victims");
}
