// CUDA kernels of the GPU render stage.  A line-for-line port of the GLSL in
// VoiceStageGLSL.h: the same row/state layouts (render/GpuBackend.h), the same
// per-frame operation order and the same warp butterfly reduction, so the two
// backends agree and both match the scalar renderer.  The scalar path's math is
// repeated step by step (cursor += step once per frame, the interior/edge
// classification), which needs no fused multiply-add: build with --fmad=false
// (CMake passes it) so nothing is contracted.  Only the cross-voice sum order
// differs from the CPU, so audio is tolerance-equal and cursor/filter state is
// bit-equal.  The cursor is 32.32 fixed point: an index word and a fraction word.
//
// One thread renders one voice, GROUP voices per block.  Each block writes one
// partial sum per frame and channel; svms_reduce adds the blocks of a part.

typedef unsigned int u32;

#define GROUP 128u
#define TILE 4u
#define ROWS (2u * TILE)
#define BATCH 4u
#define WARPS (GROUP / 32u)

static __device__ __forceinline__ float Bf(u32 v) { return __uint_as_float(v); }
static __device__ __forceinline__ u32 Fb(float v) { return __float_as_uint(v); }
static __device__ __forceinline__ int Sx16(u32 v) { return static_cast<int>(v << 16) >> 16; }
static __device__ __forceinline__ float ShflXor(float v, int mask) {
    return __shfl_xor_sync(0xFFFFFFFFu, v, mask);
}

// out = keep + shuffle(send): the lane with the mask bit set keeps the high
// value, the other keeps the low one.
#define BUTTERFLY(LOW, HIGH, MASK, UP, OUT) { float send_ = (UP) ? (LOW) : (HIGH); float keep_ = (UP) ? (HIGH) : (LOW); OUT = keep_ + ShflXor(send_, MASK); }

// Reduce-scatter one tile (TILE frames x left/right = eight values per voice)
// over the warp, then over the warps of the block once per BATCH tiles.
static __device__ __forceinline__ void ReduceTile(
    const float* ov, u32 tile, u32 tiles, u32 t, u32 outGroup, u32 frames,
    float* partial, float* shw) {
    const u32 lane32 = t & 31u;
    const u32 warp = t >> 5u;
    const bool up = (lane32 & 16u) != 0u;
    float a0, a1, a2, a3;
    BUTTERFLY(ov[0], ov[4], 16, up, a0)
    BUTTERFLY(ov[1], ov[5], 16, up, a1)
    BUTTERFLY(ov[2], ov[6], 16, up, a2)
    BUTTERFLY(ov[3], ov[7], 16, up, a3)
    const bool up8 = (lane32 & 8u) != 0u;
    float c0, c1;
    BUTTERFLY(a0, a2, 8, up8, c0)
    BUTTERFLY(a1, a3, 8, up8, c1)
    const bool up4 = (lane32 & 4u) != 0u;
    float d;
    BUTTERFLY(c0, c1, 4, up4, d)
    d += ShflXor(d, 2);
    d += ShflXor(d, 1);
    // Lanes 0, 4, ..., 28 now hold the warp sums of values 0..7 of this tile.
    const u32 inBatch = tile & (BATCH - 1u);
    const u32 buf = (tile / BATCH) & 1u;
    if ((lane32 & 3u) == 0u)
        shw[((buf * BATCH + inBatch) * WARPS + warp) * 8u + (lane32 >> 2u)] = d;
    if (inBatch == BATCH - 1u || tile == tiles - 1u) {
        __syncthreads();
        if (t < (inBatch + 1u) * 8u) {
            const u32 ti = t >> 3u;
            const u32 vi = t & 7u;
            float sum = 0.0f;
            for (u32 w = 0u; w < WARPS; ++w)
                sum += shw[((buf * BATCH + ti) * WARPS + w) * 8u + vi];
            const u32 n = (tile - inBatch + ti) * TILE + (vi >> 1u);
            if (n < frames)
                partial[(static_cast<size_t>(outGroup) * frames + n) * 2u + (vi & 1u)] = sum;
        }
    }
}

// Signed 16-bit sample `i` of the store (two samples per 32-bit word).
static __device__ __forceinline__ int Sample16(const u32* store, u32 i) {
    return static_cast<int>(store[i >> 1u] << (16u - ((i & 1u) << 4u))) >> 16;
}

// Interpolation weight of a 32-bit fraction: its top 24 bits, exact in a float
// (the same value the scalar and AVX2 kernels use).
static __device__ __forceinline__ float FxWeight(u32 frac) {
    return static_cast<float>(frac >> 8u) * (1.0f / 16777216.0f);
}

// cursor += step in 32.32 (index word, fraction word).
#define FX_ADD(PI, PF, SI, SF) { const u32 nf_ = (PF) + (SF); const u32 c_ = nf_ < (SF) ? 1u : 0u; PF = nf_; PI += (SI) + c_; }

// Wraps an index past the loop end back into the loop (any number of loops);
// the loop end has no fraction, so the fraction word is untouched.
static __device__ __forceinline__ void FxWrap(u32& idx, u32 relLoopS, u32 relLoopE) {
    if (idx >= relLoopE) {
        u32 over = idx - relLoopE;
        const u32 len = relLoopE - relLoopS;
        if (len == 0u) {
            over = 0u;
        } else if (over >= len) {
            over = over % len;
        }
        idx = relLoopS + over;
    }
}

// Position of the next frame of a sustained-loop row: wrap, then interior
// (reads idx and idx + 1) or edge (reads idx and the loop start).  Advances the
// cursor exactly once, only for frames that exist.
static __device__ __forceinline__ void Classify(
    bool v, u32& pIdx, u32& pFrac, u32 sIdx, u32 sFrac, u32& first,
    float& frac, bool& edge, u32 sampleStart, u32 relLoopS, u32 relLoopE) {
    if (v) FxWrap(pIdx, relLoopS, relLoopE);
    u32 base = pIdx;
    edge = v && (pIdx + 1u >= relLoopE);
    if (edge) base = base < relLoopE - 1u ? base : relLoopE - 1u;
    first = sampleStart + base;
    frac = FxWeight(pFrac);
    if (v) FX_ADD(pIdx, pFrac, sIdx, sFrac)
}

// Fast path: four interior frames (no clamp, no wrap): frame k reads (I, F) =
// (index, weight) of the cursor (PI, PF).
#define FAST_POS(I, F, PI, PF) { I = sampleStart + (PI); F = FxWeight(PF); }
// The two samples at positions i, i+1 as the halves of one 32-bit value,
// without a branch: shift the 64-bit window (wa, wb) right by 16 * (i & 1).
#define FAST_PAIR(I) u32 p_ = ((I) & 1u) << 4u; u32 wa_ = store[(I) >> 1u]; u32 wb_ = store[((I) >> 1u) + 1u]; u32 x_ = (wa_ >> p_) | ((wb_ << (31u - p_)) << 1u); int s0_ = Sx16(x_); int s1_ = static_cast<int>(x_) >> 16; float d_ = static_cast<float>(s1_ - s0_); float m_ = d_ * f_; float u_ = static_cast<float>(s0_) + m_;
#define FAST_OUT_PLAIN(K, I, F) { float f_ = (F); FAST_PAIR(I) ov[2u * (K)] = u_ * gainLs; ov[2u * (K) + 1u] = u_ * gainRs; }
#define FAST_OUT_FILT(K, I, F) { float f_ = (F); FAST_PAIR(I) float smp_ = u_ * kScale; float outv_ = smp_ * a0 + z1; float zin_ = smp_ * twoA0 + z2; float fb1_ = b1 * outv_; float nz1_ = zin_ - fb1_; float zin2_ = smp_ * a0; float fb2_ = b2 * outv_; z1 = nz1_; z2 = zin2_ - fb2_; ov[2u * (K)] = outv_ * gainL; ov[2u * (K) + 1u] = outv_ * gainR; }

extern "C" __global__ void __launch_bounds__(GROUP) svms_voice(
    const u32* __restrict__ store, const u32* __restrict__ rows,
    const u32* __restrict__ listv, uint4* __restrict__ dynv,
    u32* __restrict__ state, float* __restrict__ partial, u32 uCount,
    u32 uFrames, u32 uListBase, u32 uGroupBase) {
    __shared__ float shw[2u * BATCH * WARPS * 8u];
    const u32 t = threadIdx.x;
    const u32 g = blockIdx.x;
    const u32 idx = g * GROUP + t;
    const bool live = idx < uCount;

    u32 pIdx = 0u, pFrac = 0u, sIdx = 0u, sFrac = 0u;
    float z1 = 0.0f, z2 = 0.0f;
    u32 sampleStart = 0u, relLoopS = 0u, relLoopE = 1u;
    bool filt = false;
    float gainL = 0.0f, gainR = 0.0f, a0 = 0.0f, b1 = 0.0f, b2 = 0.0f;
    u32 handle = 0u;
    if (live) {
        handle = listv[uListBase + idx];
        const size_t sb = static_cast<size_t>(handle) * 9u;
        const size_t b = static_cast<size_t>(handle) * 12u;
        const u32 w0 = rows[b];
        sampleStart = w0 & 0x7FFFFFFFu;
        filt = (w0 & 0x80000000u) != 0u;
        relLoopS = rows[b + 1u];
        relLoopE = rows[b + 2u];
        sIdx = rows[b + 3u];
        sFrac = rows[b + 4u];
        gainL = Bf(rows[b + 7u]);
        gainR = Bf(rows[b + 8u]);
        a0 = Bf(rows[b + 9u]);
        b1 = Bf(rows[b + 10u]);
        b2 = Bf(rows[b + 11u]);
        pIdx = state[sb];
        pFrac = state[sb + 8u];
        z1 = Bf(state[sb + 1u]);
        z2 = Bf(state[sb + 2u]);
    }
    const float twoA0 = 2.0f * a0;
    // Samples are fetched as integers; the 1/32768 conversion is a power of
    // two, so it commutes exactly with the interpolation and folds into the
    // gains (the filter, which needs the scaled sample, applies it itself).
    const float kScale = 1.0f / 32768.0f;
    const float gainLs = gainL * kScale;
    const float gainRs = gainR * kScale;
    const u32 tiles = (uFrames + TILE - 1u) / TILE;
    for (u32 tile = 0u; tile < tiles; ++tile) {
        float ov[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        // Fast path: four interior frames (almost every tile).  The cursor is
        // exact, so the last frame's index decides whether any of the four
        // wraps or touches the loop edge.
        u32 q1i = 0u, q1f = 0u, q2i = 0u, q2f = 0u, q3i = 0u, q3f = 0u;
        bool fast = live && (tile + 1u) * TILE <= uFrames;
        if (fast) {
            FxWrap(pIdx, relLoopS, relLoopE);
            q1i = pIdx; q1f = pFrac; FX_ADD(q1i, q1f, sIdx, sFrac)
            q2i = q1i; q2f = q1f; FX_ADD(q2i, q2f, sIdx, sFrac)
            q3i = q2i; q3f = q2f; FX_ADD(q3i, q3f, sIdx, sFrac)
            fast = q3i + 1u < relLoopE;
        }
        if (fast) {
            u32 i0, i1, i2, i3;
            float f0, f1, f2, f3;
            FAST_POS(i0, f0, pIdx, pFrac) FAST_POS(i1, f1, q1i, q1f)
            FAST_POS(i2, f2, q2i, q2f) FAST_POS(i3, f3, q3i, q3f)
            pIdx = q3i; pFrac = q3f; FX_ADD(pIdx, pFrac, sIdx, sFrac)
            if (filt) {
                FAST_OUT_FILT(0u, i0, f0) FAST_OUT_FILT(1u, i1, f1)
                FAST_OUT_FILT(2u, i2, f2) FAST_OUT_FILT(3u, i3, f3)
            } else {
                FAST_OUT_PLAIN(0u, i0, f0) FAST_OUT_PLAIN(1u, i1, f1)
                FAST_OUT_PLAIN(2u, i2, f2) FAST_OUT_PLAIN(3u, i3, f3)
            }
        } else {
            // General path (run starts, edge frames, the partial last tile,
            // dead lanes), software-pipelined by one frame.
            u32 cFirst, nFirst;
            float cFrac, nFrac;
            bool cEdge, nEdge, cValid, nValid;
            u32 cA, cB, nA, nB;
            cValid = live && (tile * TILE) < uFrames;
            Classify(cValid, pIdx, pFrac, sIdx, sFrac, cFirst, cFrac, cEdge,
                     sampleStart, relLoopS, relLoopE);
            cA = store[cFirst >> 1u];
            cB = store[(cFirst >> 1u) + 1u];
#pragma unroll
            for (u32 k = 0u; k < TILE; ++k) {
                nValid = false;
                nFirst = 0u; nFrac = 0.0f; nEdge = false; nA = 0u; nB = 0u;
                if (k + 1u < TILE) {
                    nValid = live && (tile * TILE + k + 1u) < uFrames;
                    Classify(nValid, pIdx, pFrac, sIdx, sFrac, nFirst, nFrac, nEdge,
                             sampleStart, relLoopS, relLoopE);
                    nA = store[nFirst >> 1u];
                    nB = store[(nFirst >> 1u) + 1u];
                }
                float oL = 0.0f;
                float oR = 0.0f;
                if (cValid) {
                    const u32 par = (cFirst & 1u) << 4u;
                    const int s0 = static_cast<int>(cA << (16u - par)) >> 16;
                    int s1;
                    if (cEdge)
                        s1 = Sample16(store, sampleStart + relLoopS);
                    else
                        s1 = par == 0u ? (static_cast<int>(cA) >> 16)
                                       : Sx16(cB);
                    const float d = static_cast<float>(s1 - s0);
                    const float m = d * cFrac;
                    const float u = static_cast<float>(s0) + m;
                    if (filt) {
                        const float smp = u * kScale;
                        const float outv = smp * a0 + z1;
                        const float zin = smp * twoA0 + z2;
                        const float fb1 = b1 * outv;
                        const float nz1 = zin - fb1;
                        const float zin2 = smp * a0;
                        const float fb2 = b2 * outv;
                        const float nz2 = zin2 - fb2;
                        z1 = nz1;
                        z2 = nz2;
                        oL = outv * gainL;
                        oR = outv * gainR;
                    } else {
                        oL = u * gainLs;
                        oR = u * gainRs;
                    }
                }
                ov[2u * k] = oL;
                ov[2u * k + 1u] = oR;
                cValid = nValid; cFirst = nFirst; cFrac = nFrac; cEdge = nEdge;
                cA = nA; cB = nB;
            }
        }
        ReduceTile(ov, tile, tiles, t, uGroupBase + g, uFrames, partial, shw);
    }
    if (live) {
        dynv[uListBase + idx] = make_uint4(pIdx, Fb(z1), Fb(z2), pFrac);
        const size_t sb = static_cast<size_t>(handle) * 9u;
        state[sb] = pIdx;
        state[sb + 8u] = pFrac;
        state[sb + 1u] = Fb(z1);
        state[sb + 2u] = Fb(z2);
    }
}

// Envelope rows: the scalar generic span machine (RenderPrimaryVoiceSpan) for a
// fully mixed, non-faded, unrotated voice -- release tails, attack and decay
// stages and one-shots.  One frame at a time, same operation order.

// One interior frame of a released or held-sustained row: the generic machine
// with the sample pair already loaded.  `pf` is the fraction word of the cursor
// the frame reads (the caller advances the cursor).
static __device__ __forceinline__ void EnvFastFrame(
    u32 wa, u32 wb, u32 b, u32 sampleStart, u32 pf, u32 n,
    u32 stage, bool filt, float a0, float b1, float b2, float twoA0, float mixL,
    float mixR, float releaseDecay, float retireThreshold,
    float& gain, u32& relRem, float& z1, float& z2, bool& alive, u32& retire,
    float& oL, float& oR) {
    oL = 0.0f;
    oR = 0.0f;
    if (!alive) return;
    const float kScale = 1.0f / 32768.0f;
    const u32 par = ((sampleStart + b) & 1u) << 4u;
    const u32 x = (wa >> par) | ((wb << (31u - par)) << 1u);
    // int16 -> float without the integer converts: a 16-bit value v + 32768 in
    // the low mantissa bits of 2^23 is exactly 2^23 + 32768 + v, so the biased
    // difference is the exact float(s1 - s0) and the bias folds out of float(s0)
    // exactly.
    const float frac = FxWeight(pf);
    const float f0 = Bf(0x4B000000u | ((x & 0xFFFFu) ^ 0x8000u));
    const float f1 = Bf(0x4B000000u | ((x >> 16u) ^ 0x8000u));
    const float d = f1 - f0;
    const float m = d * frac;
    const float u = (f0 - 8421376.0f) + m;
    bool relFin = false;
    float g = gain;
    u32 rr = relRem;
    if (stage == 0u) {
        if (rr == 0u) {
            relFin = true;
        } else {
            g = g * releaseDecay;
            if (rr != 0xFFFFFFFFu) {
                --rr;
                relFin = rr == 0u;
            }
        }
    }
    gain = g;
    relRem = rr;
    float smp = u * kScale;
    if (filt) {
        const float outv = smp * a0 + z1;
        const float zin = smp * twoA0 + z2;
        const float fb1 = b1 * outv;
        const float nz1 = zin - fb1;
        const float zin2 = smp * a0;
        const float fb2 = b2 * outv;
        z1 = nz1;
        z2 = zin2 - fb2;
        smp = outv;
    }
    const float scaled = smp * g;
    oL = scaled * mixL;
    oR = scaled * mixR;
    if (relFin || (stage == 0u && rr == 0xFFFFFFFFu && g < retireThreshold)) {
        retire = n;
        alive = false;
    }
}

// The timeline (tlh, one uint4 per handle: start frame, release frame, flags):
// a row with a start frame renders nothing, and advances nothing, before it;
// at its release frame it flips to the released machine (stage 0, looping off
// for SF2 sampleModes 3) exactly where the CPU flips state; a row that is held
// as a sustained loop until then (kTlPreSustained) runs the scalar sustained
// span's math up to the flip, which differs from the generic machine in its
// wrap bookkeeping.  A row without a timeline is {0, 0xFFFFFFFF, 0}.
#define TL_ACTIVE 0x80000000u
#define TL_PRE_SUSTAINED 1u
#define TL_CLEAR_LOOP 2u
#define FLIPPED_BIT (1u << 28)

extern "C" __global__ void __launch_bounds__(GROUP) svms_env(
    const u32* __restrict__ store, const u32* __restrict__ erows,
    const u32* __restrict__ elist, uint4* __restrict__ edyn,
    u32* __restrict__ state, float* __restrict__ partial, u32 uCount,
    u32 uFrames, u32 uListBase, u32 uGroupBase, u32 uRetireBits,
    const uint4* __restrict__ tlh) {
    __shared__ float shw[2u * BATCH * WARPS * 8u];
    const u32 t = threadIdx.x;
    const u32 g = blockIdx.x;
    const u32 idx = g * GROUP + t;
    const bool live = idx < uCount;

    u32 pIdx = 0u, pFrac = 0u, sIdx = 0u, sFrac = 0u;
    float z1 = 0.0f, z2 = 0.0f, gain = 0.0f;
    u32 stage = 3u, relRem = 0u, atkRem = 0u, decRem = 0u;
    u32 sampleStart = 0u, relEnd = 2u, relLoopS = 0u, relLoopE = 1u;
    bool filt = false, loop = false;
    float mixL = 0.0f, mixR = 0.0f, targetGain = 0.0f, sustainLevel = 0.0f;
    float attackStep = 0.0f, decaySlope = 1.0f, releaseDecay = 1.0f;
    float a0 = 0.0f, b1 = 0.0f, b2 = 0.0f, heldL = 0.0f, heldR = 0.0f;
    u32 tlStart = 0u, tlRelease = 0xFFFFFFFFu, tlFlags = 0u;
    u32 handle = 0u;
    if (live) {
        handle = elist[uListBase + idx];
        const size_t sb = static_cast<size_t>(handle) * 9u;
        const size_t r = static_cast<size_t>(handle) * 20u;
        pIdx = state[sb];
        pFrac = state[sb + 8u];
        z1 = Bf(state[sb + 1u]);
        z2 = Bf(state[sb + 2u]);
        gain = Bf(state[sb + 3u]);
        stage = state[sb + 4u];
        atkRem = state[sb + 5u];
        decRem = state[sb + 6u];
        relRem = state[sb + 7u];
        sampleStart = erows[r];
        const u32 flags = erows[r + 1u];
        filt = (flags & 1u) != 0u;
        loop = (flags & 2u) != 0u;
        relEnd = erows[r + 2u];
        relLoopS = erows[r + 3u];
        relLoopE = erows[r + 4u];
        sIdx = erows[r + 5u];
        sFrac = erows[r + 6u];
        mixL = Bf(erows[r + 8u]);
        mixR = Bf(erows[r + 9u]);
        targetGain = Bf(erows[r + 10u]);
        sustainLevel = Bf(erows[r + 11u]);
        attackStep = Bf(erows[r + 12u]);
        decaySlope = Bf(erows[r + 13u]);
        releaseDecay = Bf(erows[r + 14u]);
        a0 = Bf(erows[r + 15u]);
        b1 = Bf(erows[r + 16u]);
        b2 = Bf(erows[r + 17u]);
        heldL = Bf(erows[r + 18u]);
        heldR = Bf(erows[r + 19u]);
        const uint4 th = tlh[handle];
        tlStart = th.x;
        tlRelease = th.y;
        tlFlags = th.z;
    }
    const float twoA0 = 2.0f * a0;
    const float kScale = 1.0f / 32768.0f;
    const float retireThreshold = Bf(uRetireBits);
    const u32 kForever = 0xFFFFFFFFu;
    const bool tlRow = (tlFlags & TL_ACTIVE) != 0u;
    const bool clearLoop = (tlFlags & TL_CLEAR_LOOP) != 0u;
    bool preSustained = (tlFlags & TL_PRE_SUSTAINED) != 0u;
    bool flipped = false;
    u32 stageAtFlip = 0u;
    float gainAtFlip = 0.0f;
    // The held machine reads render gains.
    const float heldLs = heldL * kScale;
    const float heldRs = heldR * kScale;
    bool alive = live;
    u32 retire = 0xFFFFFFu;
    const u32 tiles = (uFrames + TILE - 1u) / TILE;
    // Four-frame fast path (released and held-sustained rows): the four frames
    // of the tile are all interior (same sample pair, no wrap, no end of
    // sample), so the loads can be issued up front.  The envelope, filter and
    // retirement still run frame by frame in the generic machine's order.  Rows
    // with a timeline take the frame-by-frame path below.
    const u32 interiorLimit = loop ? relLoopE : relEnd;
    for (u32 tile = 0u; tile < tiles; ++tile) {
        float ov[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        bool fast = !tlRow && alive && (stage == 0u || stage == 3u) &&
                    (tile + 1u) * TILE <= uFrames;
        // Cursor positions of frames 1..3 and the one after the tile (exact).
        u32 a1i = pIdx, a1f = pFrac, a2i = 0u, a2f = 0u, a3i = 0u, a3f = 0u;
        u32 a4i = 0u, a4f = 0u;
        if (fast) {
            FX_ADD(a1i, a1f, sIdx, sFrac)
            a2i = a1i; a2f = a1f; FX_ADD(a2i, a2f, sIdx, sFrac)
            a3i = a2i; a3f = a2f; FX_ADD(a3i, a3f, sIdx, sFrac)
            a4i = a3i; a4f = a3f; FX_ADD(a4i, a4f, sIdx, sFrac)
            fast = a3i + 1u < interiorLimit && (!loop || a4i < relLoopE);
        }
        if (fast) {
            const u32 q0 = pIdx;
            const u32 q1 = a1i;
            const u32 q2 = a2i;
            const u32 q3 = a3i;
            const u32 i0 = (sampleStart + q0) >> 1u;
            const u32 i1 = (sampleStart + q1) >> 1u;
            const u32 i2 = (sampleStart + q2) >> 1u;
            const u32 i3 = (sampleStart + q3) >> 1u;
            const u32 wa0 = store[i0], wb0 = store[i0 + 1u];
            const u32 wa1 = store[i1], wb1 = store[i1 + 1u];
            const u32 wa2 = store[i2], wb2 = store[i2 + 1u];
            const u32 wa3 = store[i3], wb3 = store[i3 + 1u];
            float oL, oR;
            const u32 n0 = tile * TILE;
            // The cursor advances with every frame the row was alive for, the
            // retiring frame included (as in the scalar machine).
            const u32 q0f = pFrac;
            const bool live0 = alive;
            EnvFastFrame(wa0, wb0, q0, sampleStart, q0f, n0, stage, filt,
                         a0, b1, b2, twoA0, mixL, mixR, releaseDecay,
                         retireThreshold, gain, relRem, z1, z2, alive,
                         retire, oL, oR);
            ov[0] = oL; ov[1] = oR;
            if (live0) { pIdx = a1i; pFrac = a1f; }
            const bool live1 = alive;
            EnvFastFrame(wa1, wb1, q1, sampleStart, a1f, n0 + 1u, stage, filt,
                         a0, b1, b2, twoA0, mixL, mixR, releaseDecay,
                         retireThreshold, gain, relRem, z1, z2, alive,
                         retire, oL, oR);
            ov[2] = oL; ov[3] = oR;
            if (live1) { pIdx = a2i; pFrac = a2f; }
            const bool live2 = alive;
            EnvFastFrame(wa2, wb2, q2, sampleStart, a2f, n0 + 2u, stage, filt,
                         a0, b1, b2, twoA0, mixL, mixR, releaseDecay,
                         retireThreshold, gain, relRem, z1, z2, alive,
                         retire, oL, oR);
            ov[4] = oL; ov[5] = oR;
            if (live2) { pIdx = a3i; pFrac = a3f; }
            const bool live3 = alive;
            EnvFastFrame(wa3, wb3, q3, sampleStart, a3f, n0 + 3u, stage, filt,
                         a0, b1, b2, twoA0, mixL, mixR, releaseDecay,
                         retireThreshold, gain, relRem, z1, z2, alive,
                         retire, oL, oR);
            ov[6] = oL; ov[7] = oR;
            if (live3) { pIdx = a4i; pFrac = a4f; }
        } else {
#pragma unroll
            for (u32 k = 0u; k < TILE; ++k) {
                const u32 n = tile * TILE + k;
                float oL = 0.0f;
                float oR = 0.0f;
                if (alive && n < uFrames && n >= tlStart) {
                    if (n == tlRelease) {
                        // The exact-frame release flip: what the CPU does when
                        // it sets the voice Releasing between two segments.
                        flipped = true;
                        stageAtFlip = stage;
                        gainAtFlip = gain;
                        stage = 0u;
                        if (clearLoop) loop = false;
                        preSustained = false;
                    }
                    if (preSustained) {
                        // The sustained span's frame (the sustained kernel's
                        // general path): interior or edge frame.
                        u32 first;
                        float frac;
                        bool edge;
                        Classify(true, pIdx, pFrac, sIdx, sFrac, first, frac,
                                 edge, sampleStart, relLoopS, relLoopE);
                        const int s0 = Sample16(store, first);
                        const int s1 = edge
                            ? Sample16(store, sampleStart + relLoopS)
                            : Sample16(store, first + 1u);
                        const float d = static_cast<float>(s1 - s0);
                        const float m = d * frac;
                        const float u = static_cast<float>(s0) + m;
                        if (filt) {
                            const float smp = u * kScale;
                            const float outv = smp * a0 + z1;
                            const float zin = smp * twoA0 + z2;
                            const float fb1 = b1 * outv;
                            const float nz1 = zin - fb1;
                            const float zin2 = smp * a0;
                            const float fb2 = b2 * outv;
                            z1 = nz1;
                            z2 = zin2 - fb2;
                            oL = outv * heldL;
                            oR = outv * heldR;
                        } else {
                            oL = u * heldLs;
                            oR = u * heldRs;
                        }
                    } else {
                        u32 base = pIdx;
                        bool ended = false;
                        if (base + 1u >= relEnd) {
                            if (!loop) {
                                ended = true;
                            } else {
                                pIdx = relLoopS;
                                pFrac = 0u;
                                base = relLoopS;
                            }
                        }
                        if (ended) {
                            retire = n;
                            alive = false;
                        } else {
                            u32 nextRel = base + 1u;
                            if (loop && nextRel >= relLoopE) nextRel = relLoopS;
                            if (nextRel >= relEnd) nextRel = relEnd - 1u;
                            const float frac = FxWeight(pFrac);
                            const int s0 = Sample16(store, sampleStart + base);
                            const int s1 = Sample16(store, sampleStart + nextRel);
                            const float d = static_cast<float>(s1 - s0);
                            const float m = d * frac;
                            const float u = static_cast<float>(s0) + m;
                            bool relFin = false;
                            if (stage == 0u) {
                                if (relRem == 0u) {
                                    relFin = true;
                                } else {
                                    gain *= releaseDecay;
                                    if (relRem != kForever) {
                                        --relRem;
                                        relFin = relRem == 0u;
                                    }
                                }
                            } else if (stage != 3u) {
                                if (stage == 1u) {
                                    if (atkRem > 0u) {
                                        gain += attackStep;
                                        --atkRem;
                                        if (gain > targetGain) gain = targetGain;
                                    } else {
                                        gain = targetGain;
                                    }
                                    if (atkRem == 0u) stage = decRem > 0u ? 2u : 3u;
                                }
                                if (stage == 2u) {
                                    if (decRem > 0u) {
                                        gain *= decaySlope;
                                        --decRem;
                                        if (gain < sustainLevel) gain = sustainLevel;
                                    } else {
                                        gain = sustainLevel;
                                    }
                                    if (decRem == 0u) stage = 3u;
                                }
                            }
                            FX_ADD(pIdx, pFrac, sIdx, sFrac)
                            if (loop) FxWrap(pIdx, relLoopS, relLoopE);
                            float smp = u * kScale;
                            if (filt) {
                                const float outv = smp * a0 + z1;
                                const float zin = smp * twoA0 + z2;
                                const float fb1 = b1 * outv;
                                const float nz1 = zin - fb1;
                                const float zin2 = smp * a0;
                                const float fb2 = b2 * outv;
                                z1 = nz1;
                                z2 = zin2 - fb2;
                                smp = outv;
                            }
                            const float scaled = smp * gain;
                            oL = scaled * mixL;
                            oR = scaled * mixR;
                            if (relFin || (stage == 0u && relRem == kForever &&
                                           gain < retireThreshold)) {
                                retire = n;
                                alive = false;
                            }
                        }
                    }
                }
                ov[2u * k] = oL;
                ov[2u * k + 1u] = oR;
            }
        }
        ReduceTile(ov, tile, tiles, t, uGroupBase + g, uFrames, partial, shw);
    }
    if (live) {
        // A row that left its sustain stage at the flip reports its gain at the
        // flip in attackRem (the stage's counters are zero), for the CPU's
        // render-gain bake.
        const bool gainSlot = flipped && stageAtFlip == 3u;
        const u32 shownStage = flipped ? stageAtFlip : stage;
        edyn[(static_cast<size_t>(uListBase) + idx) * 3u] =
            make_uint4(pIdx, Fb(z1), Fb(z2), Fb(gain));
        edyn[(static_cast<size_t>(uListBase) + idx) * 3u + 2u] =
            make_uint4(pFrac, 0u, 0u, 0u);
        edyn[(static_cast<size_t>(uListBase) + idx) * 3u + 1u] =
            make_uint4((shownStage << 24u) | (flipped ? FLIPPED_BIT : 0u) |
                           (retire & 0xFFFFFFu),
                       gainSlot ? Fb(gainAtFlip) : atkRem,
                       gainSlot ? 0u : decRem, relRem);
        const size_t sb = static_cast<size_t>(handle) * 9u;
        state[sb] = pIdx;
        state[sb + 8u] = pFrac;
        state[sb + 1u] = Fb(z1);
        state[sb + 2u] = Fb(z2);
        state[sb + 3u] = Fb(gain);
        state[sb + 4u] = stage;
        state[sb + 5u] = atkRem;
        state[sb + 6u] = decRem;
        state[sb + 7u] = relRem;
    }
}

// Adds the partial sums of the groups [uGroupFirst, +uGroups) frame by frame,
// in group order (the order the GLSL reduction uses).
extern "C" __global__ void svms_reduce(
    const float* __restrict__ partial, float* __restrict__ mixout, u32 uFrames,
    u32 uGroups, u32 uGroupFirst, u32 uMixBase) {
    const u32 i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= uFrames * 2u) return;
    float acc = 0.0f;
    const size_t stride = static_cast<size_t>(uFrames) * 2u;
    for (u32 g = 0u; g < uGroups; ++g)
        acc += partial[(static_cast<size_t>(uGroupFirst) + g) * stride + i];
    mixout[static_cast<size_t>(uMixBase) + i] = acc;
}

// Static row records (16 words) into the handle-indexed row and state buffers.
extern "C" __global__ void svms_scatter(
    u32* __restrict__ rows, const u32* __restrict__ upload,
    u32* __restrict__ state, u32 uCount, u32 uBase) {
    const u32 i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= uCount) return;
    const size_t b = (static_cast<size_t>(uBase) + i) * 16u;
    const u32 h = upload[b];
    for (u32 w = 0u; w < 12u; ++w) rows[static_cast<size_t>(h) * 12u + w] = upload[b + 1u + w];
    for (u32 w = 0u; w < 3u; ++w) state[static_cast<size_t>(h) * 9u + w] = upload[b + 13u + w];
    state[static_cast<size_t>(h) * 9u + 8u] = upload[b + 6u];
}

// Envelope row records (36 words): statics, dynamic state and the block timeline.
extern "C" __global__ void svms_env_scatter(
    u32* __restrict__ erows, const u32* __restrict__ eupload,
    u32* __restrict__ state, uint4* __restrict__ tlh, u32 uCount, u32 uBase) {
    const u32 i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= uCount) return;
    const size_t b = (static_cast<size_t>(uBase) + i) * 36u;
    const u32 h = eupload[b];
    for (u32 w = 0u; w < 20u; ++w) erows[static_cast<size_t>(h) * 20u + w] = eupload[b + 1u + w];
    for (u32 w = 0u; w < 9u; ++w) state[static_cast<size_t>(h) * 9u + w] = eupload[b + 21u + w];
    tlh[h] = make_uint4(eupload[b + 30u], eupload[b + 31u], eupload[b + 32u], 0u);
}
