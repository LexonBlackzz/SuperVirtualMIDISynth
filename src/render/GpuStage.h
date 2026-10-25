#ifndef SVMS_GPU_STAGE_H
#define SVMS_GPU_STAGE_H

// Realtime GPU render stage: the API-neutral half.  Owns one Backend plus the
// per-block scratch (voice list, residency stamps, upload staging) that the
// whole-voice renderer fills in parallel.  RenderScalar partitions the rows
// and falls back to the CPU; this class never reads a voice row itself.
// Default OFF: with no stage attached the renderer is unchanged.

#include "render/GpuBackend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace svms {

struct GpuStageStats {
    uint64_t blocks = 0u;        // blocks that submitted GPU work
    uint64_t rows = 0u;          // sustained-loop rows rendered on the GPU
    uint64_t envRows = 0u;       // envelope rows (release, attack/decay, one-shot)
    uint64_t uploads = 0u;       // static row records uploaded
    uint64_t envUploads = 0u;    // static envelope records uploaded
    uint64_t late = 0u;          // blocks that missed the deadline (CPU fallback)
    uint64_t failed = 0u;        // blocks whose submit failed
    uint64_t submitMicros = 0u;
    uint64_t waitMicros = 0u;
    uint64_t readbackMicros = 0u;
    uint64_t classifyMicros = 0u;   // pass 1: eligibility flags
    uint64_t packMicros = 0u;       // pass 2: list + row packing
    uint64_t uploadMicros = 0u;     // static rows to the GPU
    uint64_t scatterMicros = 0u;    // read-back state into the SoA
    uint64_t sortMicros = 0u;       // ordering the lists by sample region
    uint32_t lastBlockRows = 0u; // voices on the GPU in the latest GPU block
    uint64_t considered = 0u;    // blocks the renderer offered to the stage
    // Blocks the stage declined, by reason: 0 no store / short block, 1 channel
    // buses, 2 phase rotation, 3 decimation or scalar vibrato, 4 fewer active
    // rows than the minimum, 5 stage could not prepare, 6 too few GPU-eligible
    // rows (the rest could not go to the GPU), 7 a submit failed.
    uint64_t declined[8] = {};
    bool lost = false;           // device gone; the stage is disabled
};

class GpuStage {
public:
    static constexpr uint32_t kMaxJobs = 64u;

    static std::unique_ptr<GpuStage> Create(gpu::Api api, uint32_t deviceIndex,
                                            std::string& error);

    const char* Name() const { return backend_ ? backend_->Name() : ""; }
    bool Enabled() const { return enabled_ && !stats_.lost; }
    const GpuStageStats& Stats() const { return stats_; }
    void ProfileNs(uint64_t& voice, uint64_t& reduce, uint64_t& env) const {
        backend_->ProfileNs(voice, reduce, env);
    }

    // Tunables.  minimumVoices: below it the round trip costs more than AVX2.
    void SetMinimumVoices(uint32_t voices) { minimumVoices_ = voices; }
    uint32_t MinimumVoices() const { return minimumVoices_; }
    // Blocks with at least this many GPU rows go out in two parts.
    void SetSplitMinRows(uint32_t rows) { splitMinRows_ = rows; }
    uint32_t SplitMinRows() const { return splitMinRows_; }
    // Parts a split block goes out in (classify, pack and submit one part
    // while the GPU runs the previous one).
    void SetParts(uint32_t parts) {
        parts_ = parts < 1u ? 1u : (parts > gpu::kMaxParts ? gpu::kMaxParts : parts);
    }
    uint32_t Parts() const { return parts_; }
    void SetSampleRate(uint32_t rate) { sampleRate_ = rate ? rate : 44100u; }
    // Fraction of a block period the GPU may take before the CPU falls back.
    // 0 = wait forever (tests, offline).
    void SetDeadlinePercent(uint32_t percent) { deadlinePercent_ = percent; }
    // Test hook: report the next `blocks` blocks as late.
    void ForceLateForTest(uint32_t blocks) { forceLate_ = blocks; }
    void SetMaxFramesHint(uint32_t frames) { maxFramesHint_ = frames; }
    // Orders each part's lists by sample region before submitting.  Voices that
    // read the same sample then share a warp, so their loads land on the same
    // cache lines instead of one line per lane.  Only the order the GPU sums
    // the voices in changes (the mix is tolerance-equal either way).
    void SetSortByRegion(bool on) { sortByRegion_ = on; }
    bool SortByRegion() const { return sortByRegion_; }

    // Stable counting sort of rows[0, count) by the store position each row
    // reads at block start, in 64-frame (one cache line) steps: two 12-bit
    // passes.  `sampleStart` and `phases` are the voice pool's per-handle arrays.
    void SortRows(uint32_t* rows, uint32_t count, const uint32_t* sampleStart,
                  const float* phases) {
        if (!sortByRegion_ || count < 64u || count > sortA_.size()) return;
        const uint64_t start = NowMicros();
        uint64_t* a = sortA_.data();
        uint64_t* b = sortB_.data();
        for (uint32_t i = 0u; i < count; ++i) {
            const uint32_t h = rows[i];
            const float phase = phases[h];
            const uint32_t position =
                sampleStart[h] + (phase > 0.0f && phase < 1.0e9f ? static_cast<uint32_t>(phase) : 0u);
            const uint32_t key = (std::min)(position >> 6, 0xFFFFFFu);
            a[i] = (static_cast<uint64_t>(key) << 32) | h;
        }
        uint32_t counts[4096];
        for (uint32_t shift = 32u; shift <= 44u; shift += 12u) {
            std::memset(counts, 0, sizeof(counts));
            for (uint32_t i = 0u; i < count; ++i) ++counts[(a[i] >> shift) & 4095u];
            uint32_t sum = 0u;
            for (uint32_t d = 0u; d < 4096u; ++d) {
                const uint32_t c = counts[d];
                counts[d] = sum;
                sum += c;
            }
            for (uint32_t i = 0u; i < count; ++i)
                b[counts[(a[i] >> shift) & 4095u]++] = a[i];
            std::swap(a, b);
        }
        for (uint32_t i = 0u; i < count; ++i) rows[i] = static_cast<uint32_t>(a[i]);
        stats_.sortMicros += NowMicros() - start;
    }

    // Anything that rewrites a resident row's static fields outside the
    // whole-voice plan (sample-store swap, pool rebuild, pitch refresh, ...)
    // calls this; the next block re-uploads every row.  Callable from any
    // thread.
    void BumpEpoch() { epoch_.fetch_add(1u, std::memory_order_relaxed); }
    // One row's static fields changed under its play index (audio thread,
    // between Prepare calls): re-upload it next time it is eligible.
    void ForgetRow(uint32_t handle) {
        if (handle < resident_.size()) resident_[handle] = UINT32_MAX;
    }

    // Called by the renderer at the start of every candidate block.  Uploads
    // the sample store when it changed, sizes the buffers, clears residency on
    // an epoch change.  False = the stage cannot serve this block.
    bool Prepare(const int16_t* store, uint32_t storeFrames,
                 uint32_t handleCapacity, uint32_t maxFrames) {
        if (!Enabled() || store == nullptr || storeFrames == 0u ||
            handleCapacity == 0u || maxFrames == 0u) {
            return false;
        }
        bool clearResidency = false;
        if (handleCapacity != handleCapacity_ || maxFrames > maxFrames_) {
            const uint32_t frames =
                (std::max)((std::max)(maxFrames, maxFrames_), maxFramesHint_);
            try {
                resident_.assign(handleCapacity, UINT32_MAX);
                residentKind_.assign(handleCapacity, 0u);
                flags_.assign(handleCapacity, 0u);
                list_.assign(handleCapacity, 0u);
                upload_.assign(handleCapacity, gpu::UploadRecord{});
                envList_.assign(handleCapacity, 0u);
                envUpload_.assign(handleCapacity, gpu::EnvUploadRecord{});
                retire_.assign(handleCapacity, RetireRec{});
                cross_.assign(handleCapacity, 0u);
                cpuRows_.assign(handleCapacity, 0u);
                sortA_.assign(handleCapacity, 0u);
                sortB_.assign(handleCapacity, 0u);
            } catch (...) {
                enabled_ = false;
                return false;
            }
            if (!backend_->Reserve(handleCapacity, frames)) {
                enabled_ = false;
                return false;
            }
            handleCapacity_ = handleCapacity;
            maxFrames_ = frames;
            clearResidency = true;
        }
        if (store != storePtr_ || storeFrames != storeFrames_) {
            if (!backend_->UploadStore(store, storeFrames)) {
                enabled_ = false;
                return false;
            }
            storePtr_ = store;
            storeFrames_ = storeFrames;
            clearResidency = true;
        }
        const uint32_t epoch = epoch_.load(std::memory_order_relaxed);
        if (epoch != epochSeen_) {
            epochSeen_ = epoch;
            clearResidency = true;
        }
        if (clearResidency)
            std::fill(resident_.begin(), resident_.end(), UINT32_MAX);
        return true;
    }

    // Per-block scratch (valid after a successful Prepare).
    // Row kinds in Flags(): 0 = CPU, kKindSustained, kKindEnvelope.  A row's
    // residency is valid only for the kind it was uploaded as.
    static constexpr uint8_t kKindSustained = 1u;
    static constexpr uint8_t kKindEnvelope = 2u;
    // Flag bit: the row was classified from scratch, so pass 2 must compare it
    // with its resident copy (a quick-classified resident row needs nothing).
    static constexpr uint8_t kNeedsCheck = 0x80u;
    struct RetireRec {
        uint32_t handle;
        uint32_t frame;
    };
    uint32_t* Resident() { return resident_.data(); }
    uint8_t* ResidentKind() { return residentKind_.data(); }
    uint8_t* Flags() { return flags_.data(); }
    uint32_t* List() { return list_.data(); }
    uint32_t* EnvList() { return envList_.data(); }
    gpu::UploadRecord* UploadScratch() { return upload_.data(); }
    gpu::EnvUploadRecord* EnvUploadScratch() { return envUpload_.data(); }
    RetireRec* RetireScratch() { return retire_.data(); }
    uint32_t* CrossScratch() { return cross_.data(); }
    // The CPU rows of a block that has GPU rows (the CPU job stage reads them).
    uint32_t* CpuRows() { return cpuRows_.data(); }
    uint32_t* JobCrossCount() { return jobCross_; }
    uint32_t* JobGpuCount() { return jobGpu_; }
    uint32_t* JobEnvCount() { return jobEnv_; }
    uint32_t* JobUploadCount() { return jobUpload_; }
    uint32_t* JobEnvUploadCount() { return jobEnvUpload_; }
    uint32_t* JobRetireCount() { return jobRetire_; }
    const gpu::DynOut* Dyn() const { return dynOut_; }
    const gpu::EnvDynOut* EnvDyn() const { return envDynOut_; }
    const float* Mix() const { return mixOut_; }

    bool UploadRows(const gpu::UploadRecord* records, uint32_t count) {
        return backend_->UploadRows(records, count);
    }
    bool UploadEnvRows(const gpu::EnvUploadRecord* records, uint32_t count) {
        return backend_->UploadEnvRows(records, count);
    }
    // Starts a part of the block: entries [listOffset, +listCount) of the
    // sustained list and [envOffset, +envCount) of the envelope list.  A block
    // is one or two parts (the second is packed while the GPU runs the first);
    // `last` ends it.  `uploads` / `envUploads` are the numbers of static
    // records already sent for the part (counters only).
    bool SubmitPart(uint32_t listOffset, uint32_t listCount, uint32_t envOffset,
                    uint32_t envCount, uint32_t frames, uint32_t uploads,
                    uint32_t envUploads, float retireThreshold, bool first,
                    bool last) {
        const uint64_t start = NowMicros();
        if (first) submitStart_ = start;
        if (!backend_->Submit(list_.data() + listOffset, listCount,
                              envList_.data() + envOffset, envCount, frames,
                              retireThreshold, last)) {
            ++stats_.failed;
            return false;
        }
        if (first) {
            ++stats_.blocks;
            stats_.lastBlockRows = 0u;
        }
        stats_.lastBlockRows += listCount + envCount;
        stats_.rows += listCount;
        stats_.envRows += envCount;
        stats_.uploads += uploads;
        stats_.envUploads += envUploads;
        stats_.submitMicros += NowMicros() - start;
        return true;
    }
    // Drops the parts still in flight (the caller renders their rows on the
    // CPU).
    void Abandon() { backend_->Abandon(); }
    // Test hooks: report the next `blocks` blocks late at their first / second
    // part.
    void ForceLateSecondForTest(uint32_t blocks) { forceLateSecond_ = blocks; }
    // Waits for the oldest part (deadline from the block period, measured from
    // the first submit).  Ok = Mix() is that part's mix and Dyn()/EnvDyn()
    // hold its entries.
    gpu::Result Collect(uint32_t frames, uint32_t part) {
        uint64_t timeout = ~0ull;
        if (deadlinePercent_ != 0u) {
            const uint64_t budget = static_cast<uint64_t>(frames) * 1000000ull *
                                    deadlinePercent_ / (100ull * sampleRate_);
            const uint64_t spent = NowMicros() - submitStart_;
            timeout = budget > spent ? budget - spent : 0u;
        }
        uint32_t& forced = part == 0u ? forceLate_ : forceLateSecond_;
        if (forced != 0u) {
            // Drain the part so the buffers stay consistent, then report it late.
            --forced;
            backend_->Collect(~0ull, &mixOut_, &dynOut_, &envDynOut_);
            ++stats_.late;
            return gpu::Result::Late;
        }
        const uint64_t waitStart = NowMicros();
        const gpu::Result result =
            backend_->Collect(timeout, &mixOut_, &dynOut_, &envDynOut_);
        const uint64_t waited = NowMicros() - waitStart;
        if (result == gpu::Result::Ok) {
            stats_.waitMicros += waited;
        } else if (result == gpu::Result::Late) {
            ++stats_.late;
            stats_.waitMicros += waited;
        } else if (result == gpu::Result::Lost) {
            stats_.lost = true;
        }
        return result;
    }

private:
public:
    static uint64_t NowMicros() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    std::unique_ptr<gpu::Backend> backend_;
    bool enabled_ = true;
    uint32_t minimumVoices_ = 4096u;
    uint32_t splitMinRows_ = 8192u;
    uint32_t parts_ = 3u;
    uint32_t sampleRate_ = 44100u;
    uint32_t deadlinePercent_ = 0u;
    uint32_t forceLate_ = 0u;
    uint32_t forceLateSecond_ = 0u;
    uint32_t maxFramesHint_ = 0u;
    std::atomic<uint32_t> epoch_{1u};
    uint32_t epochSeen_ = 0u;
    const int16_t* storePtr_ = nullptr;
    uint32_t storeFrames_ = 0u;
    uint32_t handleCapacity_ = 0u;
    uint32_t maxFrames_ = 0u;
    std::vector<uint32_t> resident_;
    std::vector<uint8_t> residentKind_;
    std::vector<uint8_t> flags_;
    std::vector<uint32_t> list_;
    std::vector<uint32_t> envList_;
    std::vector<gpu::UploadRecord> upload_;
    std::vector<gpu::EnvUploadRecord> envUpload_;
    const float* mixOut_ = nullptr;
    const gpu::DynOut* dynOut_ = nullptr;
    const gpu::EnvDynOut* envDynOut_ = nullptr;
    std::vector<RetireRec> retire_;
    std::vector<uint32_t> cross_;
    std::vector<uint32_t> cpuRows_;
    std::vector<uint64_t> sortA_;
    std::vector<uint64_t> sortB_;
    bool sortByRegion_ = true;
    uint32_t jobGpu_[kMaxJobs] = {};
    uint32_t jobEnv_[kMaxJobs] = {};
    uint32_t jobUpload_[kMaxJobs] = {};
    uint32_t jobEnvUpload_[kMaxJobs] = {};
    uint32_t jobRetire_[kMaxJobs] = {};
    uint32_t jobCross_[kMaxJobs] = {};
public:
    GpuStageStats& MutableStats() { return stats_; }
private:
    GpuStageStats stats_;
    uint64_t submitStart_ = 0u;
};

} // namespace svms

#endif
