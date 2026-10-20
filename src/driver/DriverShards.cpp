// Key-sharded engine: routing, shard lifecycle and the per-callback fan-out.
// See the "Key-sharded engine" block in DriverInternal.h.

#include "driver/DriverInternal.h"

namespace svms {

uint32_t Driver::ShardFor(uint8_t channel, uint8_t note) const noexcept {
    // Multiplicative hash: a plain modulo of (channel,key) sent the whole
    // synthetic stream to one shard in the bench.
    const uint32_t key = static_cast<uint32_t>(channel) * 128u + note;
    return ((key * 2654435761u) >> 16u) % shardCount_;
}

// Public entry. On a sharded master, note events go to the shard that owns
// their (channel,key); everything else (controllers, program, bend, SysEx,
// reset, internal messages) is delivered to every core in the same order.
// Each core keeps its own sequence space and fences.
bool Driver::SubmitShortMsgAtQpcCancellable(
    uint32_t msg, uint64_t qpcTimestamp,
    const std::atomic<uint64_t>* externalCancellation,
    uint64_t cancellationToken) {
    if (shardCount_ <= 1u || shardMaster_)
        return SubmitShortMsgAtQpcCancellableCore(
            msg, qpcTimestamp, externalCancellation, cancellationToken);
    const uint8_t status = static_cast<uint8_t>(msg & 0xffu);
    const uint8_t type = status & 0xf0u;
    if (!IsInternalEngineMessage(msg) && (type == 0x80u || type == 0x90u)) {
        const uint32_t target = ShardFor(
            status & 0x0fu, static_cast<uint8_t>((msg >> 8u) & 0x7fu));
        Driver* core = target == 0u ? this : shards_[target - 1u];
        return core->SubmitShortMsgAtQpcCancellableCore(
            msg, qpcTimestamp, externalCancellation, cancellationToken);
    }
    bool ok = SubmitShortMsgAtQpcCancellableCore(
        msg, qpcTimestamp, externalCancellation, cancellationToken);
    for (Driver* core : shards_) {
        ok = core->SubmitShortMsgAtQpcCancellableCore(
                 msg, qpcTimestamp, externalCancellation,
                 cancellationToken) && ok;
    }
    return ok;
}

bool Driver::CreateShards() {
    for (uint32_t index = 1u; index < shardCount_; ++index) {
        Driver* core = new (std::nothrow) Driver();
        if (!core) return false;
        core->shardMaster_ = this;
        core->shardIndex_ = index;
        core->shardCount_ = shardCount_;
        shards_.push_back(core);  // owned from here: StopShards cleans up
        if (!core->Initialize()) {
            LOG("Shard %u failed to initialize", index);
            return false;
        }
        core->shardStartEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        core->shardDoneEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        core->shardOutput_ = static_cast<float*>(_aligned_malloc(
            8192u * 2u * sizeof(float), kMixBufferAlign));
        if (!core->shardStartEvent_ || !core->shardDoneEvent_ ||
            !core->shardOutput_)
            return false;
        // Adopt the bank the master already has (a SoundFont activated
        // before the shards existed); later activations fan out from
        // ActivatePendingSoundFontAtBlockBoundary.
        if (activeSoundFontStack_) core->ApplySoundFontStack(activeSoundFontStack_);
        try {
            core->shardThread_ = std::thread(&Driver::ShardThreadLoop, core);
        } catch (...) {
            return false;
        }
    }
    return true;
}

void Driver::StopShards() {
    for (Driver* core : shards_) {
        if (core->shardThread_.joinable()) {
            core->shardStop_.store(true, std::memory_order_release);
            SetEvent(core->shardStartEvent_);
            core->shardThread_.join();
        }
        if (core->shardStartEvent_) CloseHandle(core->shardStartEvent_);
        if (core->shardDoneEvent_) CloseHandle(core->shardDoneEvent_);
        core->shardStartEvent_ = core->shardDoneEvent_ = nullptr;
        core->Shutdown();
        _aligned_free(core->shardOutput_);
        core->shardOutput_ = nullptr;
        delete core;
    }
    shards_.clear();
}

void Driver::ShardThreadLoop() {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        WaitForSingleObject(shardStartEvent_, INFINITE);
        if (shardStop_.load(std::memory_order_acquire)) break;
        RenderCallback(shardOutput_, shardFrames_, this);
        SetEvent(shardDoneEvent_);
    }
}

// Master, top of the callback (after its own clock/recovery settled): bring
// every core onto the same timeline and live state, then start them.
void Driver::KickShards(uint32_t numFrames) {
    for (Driver* core : shards_) {
        if (!core->clockInitialized) {
            core->virtualRenderClockQPC = virtualRenderClockQPC;
            core->clockInitialized = true;
            core->compilerEpochQPC_.store(virtualRenderClockQPC,
                                          std::memory_order_release);
            core->compilerWakeEpoch_.fetch_add(1, std::memory_order_release);
            WakeAddressWaiters(core->compilerWakeEpoch_);
        }
        if (core->configSnapshot && configSnapshot)
            *core->configSnapshot = *configSnapshot;
        if (core->shardSyncedSeq_ != lastAppliedLiveSeq_) {
            core->shardSyncedSeq_ = lastAppliedLiveSeq_;
            core->correctnessMode_ = correctnessMode_;
            if (core->channelCache)
                core->channelCache->SetMasterVolume(
                    appliedMailbox_.masterVolume * core->sysexMasterVolume_);
            if (core->voiceManager)
                core->voiceManager->SetPhaseRotationMode(
                    appliedMailbox_.phaseRotationMode);
        }
        core->shardFrames_ = numFrames;
        SetEvent(core->shardStartEvent_);
    }
}

// Master, right after its own RenderBlock: wait for the cores and add their
// raw mixes into the master's left/right buffers. The master then
// interleaves and applies reverb/limiter to the sum.
void Driver::JoinShardsAndMix(float* left, float* right, uint32_t numFrames) {
    HANDLE done[32];
    uint32_t count = 0u;
    for (Driver* core : shards_) done[count++] = core->shardDoneEvent_;
    if (count == 0u) return;
    if (WaitForMultipleObjects(count, done, TRUE, 5000u) != WAIT_OBJECT_0) {
        LOG("Shard join timed out; block rendered without the late cores");
        return;
    }
    for (Driver* core : shards_) {
        const float* in = core->shardOutput_;  // interleaved L,R
        for (uint32_t i = 0u; i < numFrames; ++i) {
            left[i] += in[i * 2u];
            right[i] += in[i * 2u + 1u];
        }
    }
}

uint32_t Driver::ShardsActiveVoices() const noexcept {
    uint32_t total = 0u;
    for (const Driver* core : shards_)
        if (core->voiceManager) total += core->voiceManager->activeCount_;
    return total;
}

uint32_t Driver::ShardsMaxVoices() const noexcept {
    uint32_t total = 0u;
    for (const Driver* core : shards_)
        if (core->voiceManager) total += core->voiceManager->GetMaxVoices();
    return total;
}

uint32_t Driver::ShardsSteals() const noexcept {
    uint32_t total = 0u;
    for (const Driver* core : shards_)
        if (core->voiceManager) total += core->voiceManager->stealCount_;
    return total;
}

}  // namespace svms
