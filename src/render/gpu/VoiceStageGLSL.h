#ifndef SVMS_GPU_VOICE_STAGE_GLSL_H
#define SVMS_GPU_VOICE_STAGE_GLSL_H

// The one GLSL source of the GPU render stage (GLSL 4.30 compute).  The
// backend prepends "#version 430", GROUP/TILE sizes and one of VOICE_CS /
// REDUCE_CS / SCATTER_CS.  Boundary 3: the voice kernel repeats the scalar
// sustained-loop math step by step (phase += step once per frame, the same
// wrap and interior-run bookkeeping, `precise` on every value so no compiler
// fuses mul+add); only the cross-voice sum order differs, so audio is
// tolerance-equal and phase/filter state is bit-equal.
//
// Memory: a voice reads two neighbouring samples per frame from its own spot
// of a multi-megabyte store, so a warp's loads land on ~32 cache lines.  A
// per-voice shared-memory block cache (one 16-byte refill per 8 frames) was
// tried and measured slower than the plain 32-bit loads: its refill branches
// diverge every frame.  The store is read as 32-bit words.

namespace svms {
namespace gpu {

inline const char* const kVoiceStageGLSL = R"GLSL(
#if defined(VOICE_CS) || defined(ENV_CS)
layout(local_size_x = GROUP) in;
#else
layout(local_size_x = 64) in;
#endif

#if defined(VOICE_CS) || defined(ENV_CS)
#if TILE != 4
#error TILE must be 4 (the fast path is written out for four frames)
#endif
#define ROWS (2u * TILE)
#define LANES (GROUP / ROWS)
layout(std430, binding = 0) readonly buffer StoreBuf { uint store[]; };
layout(std430, binding = 3) writeonly buffer PartialBuf { float partial[]; };
uniform uint uCount;
uniform uint uFrames;

// ROWS rows = TILE frames x (left, right); one column per voice of the group.
shared float sh[ROWS * GROUP];
shared float sh2[ROWS * LANES];
uint StoreWord(uint wi) { return store[wi]; }

#if defined(SUBGROUP32)
// The tile stays in eight scalar registers (ov0..ov7 = 2 * frame + channel); a
// warp reduces it with a reduce-scatter butterfly (nine shuffles for the eight
// values instead of forty) and the warps of a group meet in shared memory once
// per BATCH tiles.  Everything is unrolled by hand: the driver keeps dynamically
// indexed arrays in local memory, which cost more than the reduction itself.
#define DECLARE_OV precise float ov0 = 0.0, ov1 = 0.0, ov2 = 0.0, ov3 = 0.0, ov4 = 0.0, ov5 = 0.0, ov6 = 0.0, ov7 = 0.0;
#define STORE_OUT(K, L, R) { precise float l_ = (L); precise float r_ = (R); if ((K) == 0u) { ov0 = l_; ov1 = r_; } else if ((K) == 1u) { ov2 = l_; ov3 = r_; } else if ((K) == 2u) { ov4 = l_; ov5 = r_; } else { ov6 = l_; ov7 = r_; } }
#define REDUCE_ARGS ov0, ov1, ov2, ov3, ov4, ov5, ov6, ov7
#define BATCH 4u
shared float shw[2u * BATCH * (GROUP / 32u) * 8u];

// out = keep + shuffle(send): the lane with the mask bit set keeps the high
// value, the other keeps the low one.
#define BUTTERFLY(LOW, HIGH, MASK, UP, OUT) { precise float send_ = (UP) ? (LOW) : (HIGH); precise float keep_ = (UP) ? (HIGH) : (LOW); OUT = keep_ + subgroupShuffleXor(send_, MASK); }

void ReduceTile(float v0, float v1, float v2, float v3, float v4, float v5,
                float v6, float v7, uint tile, uint tiles, uint t,
                uint outGroup) {
    uint lane32 = gl_SubgroupInvocationID;
    bool up = (lane32 & 16u) != 0u;
    precise float a0_, a1_, a2_, a3_;
    BUTTERFLY(v0, v4, 16u, up, a0_)
    BUTTERFLY(v1, v5, 16u, up, a1_)
    BUTTERFLY(v2, v6, 16u, up, a2_)
    BUTTERFLY(v3, v7, 16u, up, a3_)
    bool up8 = (lane32 & 8u) != 0u;
    precise float c0_, c1_;
    BUTTERFLY(a0_, a2_, 8u, up8, c0_)
    BUTTERFLY(a1_, a3_, 8u, up8, c1_)
    bool up4 = (lane32 & 4u) != 0u;
    precise float d;
    BUTTERFLY(c0_, c1_, 4u, up4, d)
    d += subgroupShuffleXor(d, 2u);
    d += subgroupShuffleXor(d, 1u);
    // Lanes 0, 4, ..., 28 now hold the warp sums of values 0..7 of this tile.
    uint inBatch = tile & (BATCH - 1u);
    uint buf = (tile / BATCH) & 1u;
    if ((lane32 & 3u) == 0u)
        shw[((buf * BATCH + inBatch) * (GROUP / 32u) + gl_SubgroupID) * 8u +
            (lane32 >> 2u)] = d;
    if (inBatch == BATCH - 1u || tile == tiles - 1u) {
        barrier();
        if (t < (inBatch + 1u) * 8u) {
            uint ti = t >> 3u;
            uint vi = t & 7u;
            precise float sum = 0.0;
            for (uint w = 0u; w < GROUP / 32u; ++w)
                sum += shw[((buf * BATCH + ti) * (GROUP / 32u) + w) * 8u + vi];
            uint n = (tile - inBatch + ti) * TILE + (vi >> 1u);
            if (n < uFrames)
                partial[(outGroup * uFrames + n) * 2u + (vi & 1u)] = sum;
        }
    }
}
#else
#define STORE_OUT(K, L, R) sh[(2u * (K)) * GROUP + t] = (L); sh[(2u * (K) + 1u) * GROUP + t] = (R);
#endif

// Signed 16-bit sample `i` of the store (two samples per 32-bit word).
int Sample16(uint i) {
    return bitfieldExtract(int(StoreWord(i >> 1u)), int((i & 1u) << 4u), 16);
}
#endif

#if defined(VOICE_CS)
layout(std430, binding = 1) readonly buffer RowBuf { uint rows[]; };
layout(std430, binding = 2) readonly buffer ListBuf { uint listv[]; };
layout(std430, binding = 5) writeonly buffer DynBuf { vec4 dynv[]; };
layout(std430, binding = 11) buffer StateBuf { uint state[]; };

// Fast path: one interior frame of a running span (no clamp, no wrap).
#define FAST_POS(I, F) { uint b_ = uint(phase); I = sampleStart + b_; F = phase - float(b_); phase += phaseInc; }
// The two samples at positions i, i+1 as the halves of one 32-bit value,
// without a branch: shift the 64-bit window (wa, wb) right by 16 * (i & 1).
#define FAST_LOAD(I) uint wa_ = store[(I) >> 1u]; uint wb_ = store[((I) >> 1u) + 1u];
#define FAST_PAIR(I) uint p_ = ((I) & 1u) << 4u; FAST_LOAD(I) uint x_ = (wa_ >> p_) | ((wb_ << (31u - p_)) << 1u); int s0_ = (int(x_) << 16) >> 16; int s1_ = int(x_) >> 16; precise float d_ = float(s1_ - s0_); precise float m_ = d_ * f_; precise float u_ = float(s0_) + m_;
#define FAST_OUT_PLAIN(K, I, F) { precise float f_ = (F); FAST_PAIR(I) STORE_OUT(K, u_ * gainLs, u_ * gainRs) }
#define FAST_OUT_FILT(K, I, F) { precise float f_ = (F); FAST_PAIR(I) precise float smp_ = u_ * kScale; precise float outv_ = smp_ * a0 + z1; precise float zin_ = smp_ * twoA0 + z2; precise float fb1_ = b1 * outv_; precise float nz1_ = zin_ - fb1_; precise float zin2_ = smp_ * a0; precise float fb2_ = b2 * outv_; z1 = nz1_; z2 = zin2_ - fb2_; STORE_OUT(K, outv_ * gainL, outv_ * gainR) }

// Position of the next frame: the scalar kernel's run bookkeeping.  Advances
// phase (and the run counter) exactly once, only for frames that exist.
void Classify(bool v, inout precise float phase, inout uint runLeft,
              out uint first, out float frac, out bool edge,
              uint sampleStart, uint relLoopE, float relLoopSF,
              float relLoopEF, float loopLength, float lastInterior,
              float phaseInc) {
    if (v && runLeft == 0u) {
        if (phase >= relLoopEF) {
            precise float overflow = phase - relLoopEF;
            if (overflow >= loopLength)
                overflow -= floor(overflow / loopLength) * loopLength;
            phase = relLoopSF + overflow;
        }
        if (phase < lastInterior) {
            precise float distance = lastInterior - phase;
            precise float runF = ceil(distance / phaseInc);
            runLeft = max(uint(min(runF, 4.0e9)), 1u);
        }
    }
    bool inRun = runLeft != 0u;
    uint base = uint(phase);
    if (!inRun) base = min(base, relLoopE - 1u);
    edge = v && !inRun;
    if (v && inRun) --runLeft;
    first = sampleStart + base;
    frac = phase - float(base);
    if (v) phase += phaseInc;
}

void main() {
    uint t = gl_LocalInvocationID.x;
    uint g = gl_WorkGroupID.x;
    uint idx = g * GROUP + t;
    bool live = idx < uCount;

    precise float phase = 0.0;
    precise float z1 = 0.0;
    precise float z2 = 0.0;
    uint sampleStart = 0u;
    uint relLoopS = 0u;
    uint relLoopE = 1u;
    bool filt = false;
    precise float relLoopSF = 0.0;
    precise float relLoopEF = 1.0;
    precise float phaseInc = 0.0;
    precise float gainL = 0.0;
    precise float gainR = 0.0;
    precise float a0 = 0.0;
    precise float b1 = 0.0;
    precise float b2 = 0.0;
    uint handle = 0u;
    if (live) {
        handle = listv[idx];
        uint sb = handle * 8u;
        uint b = handle * 12u;
        uint w0 = rows[b];
        sampleStart = w0 & 0x7FFFFFFFu;
        filt = (w0 & 0x80000000u) != 0u;
        relLoopS = rows[b + 1u];
        relLoopE = rows[b + 2u];
        relLoopSF = uintBitsToFloat(rows[b + 3u]);
        relLoopEF = uintBitsToFloat(rows[b + 4u]);
        phaseInc = uintBitsToFloat(rows[b + 6u]);
        gainL = uintBitsToFloat(rows[b + 7u]);
        gainR = uintBitsToFloat(rows[b + 8u]);
        a0 = uintBitsToFloat(rows[b + 9u]);
        b1 = uintBitsToFloat(rows[b + 10u]);
        b2 = uintBitsToFloat(rows[b + 11u]);
        phase = max(0.0, uintBitsToFloat(state[sb]));
        z1 = uintBitsToFloat(state[sb + 1u]);
        z2 = uintBitsToFloat(state[sb + 2u]);
    }
    precise float loopLength = relLoopEF - relLoopSF;
    precise float lastInterior = relLoopEF - 1.0;
    precise float twoA0 = 2.0 * a0;
    // Samples are fetched as integers; the 1/32768 conversion is a power of
    // two, so it commutes exactly with the interpolation and folds into the
    // gains (the filter, which needs the scaled sample, applies it itself).
    const float kScale = 1.0 / 32768.0;
    precise float gainLs = gainL * kScale;
    precise float gainRs = gainR * kScale;
    // The scalar kernel classifies frames in runs: at a run start it measures
    // the distance to the last interior phase once (ceil(distance / step)) and
    // renders that many interior frames with no wrap check, even if the
    // accumulated phase has drifted past the boundary by then.  Mirror it.
    uint runLeft = 0u;
    
    uint row = t / LANES;
    uint lane = t % LANES;
    uint tiles = (uFrames + TILE - 1u) / TILE;
    for (uint tile = 0u; tile < tiles; ++tile) {
#if defined(SUBGROUP32)
        DECLARE_OV
#endif
        // Fast path: four interior frames of a running span (almost every
        // tile).  No wrap, edge or tail bookkeeping is needed.
        bool fast = live && (tile + 1u) * TILE <= uFrames && runLeft >= TILE;
        if (fast) {
            uint i0, i1, i2, i3;
            float f0, f1, f2, f3;
            FAST_POS(i0, f0) FAST_POS(i1, f1) FAST_POS(i2, f2) FAST_POS(i3, f3)
            runLeft -= TILE;
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
            uint cFirst, nFirst;
            float cFrac, nFrac;
            bool cEdge, nEdge, cValid, nValid;
            uint cA, cB, nA, nB;
            cValid = live && (tile * TILE) < uFrames;
            Classify(cValid, phase, runLeft, cFirst, cFrac, cEdge, sampleStart,
                     relLoopE, relLoopSF, relLoopEF, loopLength, lastInterior,
                     phaseInc);
            cA = StoreWord(cFirst >> 1u);
            cB = StoreWord((cFirst >> 1u) + 1u);
            for (uint k = 0u; k < TILE; ++k) {
                nValid = false;
                nFirst = 0u; nFrac = 0.0; nEdge = false; nA = 0u; nB = 0u;
                if (k + 1u < TILE) {
                    nValid = live && (tile * TILE + k + 1u) < uFrames;
                    Classify(nValid, phase, runLeft, nFirst, nFrac, nEdge,
                             sampleStart, relLoopE, relLoopSF, relLoopEF,
                             loopLength, lastInterior, phaseInc);
                    nA = StoreWord(nFirst >> 1u);
                    nB = StoreWord((nFirst >> 1u) + 1u);
                }
                precise float oL = 0.0;
                precise float oR = 0.0;
                if (cValid) {
                    uint par = (cFirst & 1u) << 4u;
                    int s0 = bitfieldExtract(int(cA), int(par), 16);
                    int s1;
                    if (cEdge)
                        s1 = Sample16(sampleStart + relLoopS);
                    else
                        s1 = par == 0u ? bitfieldExtract(int(cA), 16, 16)
                                       : bitfieldExtract(int(cB), 0, 16);
                    precise float d = float(s1 - s0);
                    precise float m = d * cFrac;
                    precise float u = float(s0) + m;
                    if (filt) {
                        precise float smp = u * kScale;
                        precise float outv = smp * a0 + z1;
                        precise float zin = smp * twoA0 + z2;
                        precise float fb1 = b1 * outv;
                        precise float nz1 = zin - fb1;
                        precise float zin2 = smp * a0;
                        precise float fb2 = b2 * outv;
                        precise float nz2 = zin2 - fb2;
                        z1 = nz1;
                        z2 = nz2;
                        oL = outv * gainL;
                        oR = outv * gainR;
                    } else {
                        oL = u * gainLs;
                        oR = u * gainRs;
                    }
                }
                STORE_OUT(k, oL, oR)
                cValid = nValid; cFirst = nFirst; cFrac = nFrac; cEdge = nEdge;
                cA = nA; cB = nB;
            }
        }
#if defined(SUBGROUP32)
        ReduceTile(REDUCE_ARGS, tile, tiles, t, g);
#else
        barrier();
        {
            precise float acc = 0.0;
            for (uint j = 0u; j < ROWS; ++j)
                acc += sh[row * GROUP + j * LANES + lane];
            sh2[row * LANES + lane] = acc;
        }
        barrier();
        if (lane == 0u) {
            precise float sum = 0.0;
            for (uint j = 0u; j < LANES; ++j)
                sum += sh2[row * LANES + j];
            uint n = tile * TILE + (row >> 1u);
            if (n < uFrames)
                partial[(g * uFrames + n) * 2u + (row & 1u)] = sum;
        }
#endif
    }
    if (live) {
        dynv[idx] = vec4(phase, z1, z2, 0.0);
        uint sb = handle * 8u;
        state[sb] = floatBitsToUint(phase);
        state[sb + 1u] = floatBitsToUint(z1);
        state[sb + 2u] = floatBitsToUint(z2);
    }
}
#endif

// Envelope rows: the scalar generic span machine (RenderPrimaryVoiceSpan)
// for a fully mixed, non-faded, unrotated voice -- release tails, attack and
// decay stages and one-shots.  One frame at a time, same operation order.
#if defined(ENV_CS)
layout(std430, binding = 7) readonly buffer EnvRowBuf { uint erows[]; };
layout(std430, binding = 8) readonly buffer EnvListBuf { uint elist[]; };
layout(std430, binding = 9) writeonly buffer EnvDynBuf { uvec4 edyn[]; };
layout(std430, binding = 11) buffer StateBuf { uint state[]; };
uniform uint uGroupBase;
uniform uint uRetireBits;
#define EMIT(K, L, R) STORE_OUT(K, L, R)

// One interior frame of a released or held-sustained row: the generic machine
// with the sample pair already loaded.  `ph` is the phase the frame reads and
// `phNext` the phase after it advances.
void EnvFastFrame(uint wa, uint wb, uint b, uint sampleStart, float ph,
                  float phNext, uint n, uint stage, bool filt, float a0,
                  float b1, float b2, float twoA0, float mixL, float mixR,
                  float releaseDecay, float retireThreshold,
                  inout float phase, inout float gain, inout uint relRem,
                  inout float z1, inout float z2, inout bool alive,
                  inout uint retire, out float oL, out float oR) {
    oL = 0.0;
    oR = 0.0;
    if (!alive) return;
    const float kScale = 1.0 / 32768.0;
    uint par = ((sampleStart + b) & 1u) << 4u;
    uint x = (wa >> par) | ((wb << (31u - par)) << 1u);
    // int16 -> float without the integer converts: a 16-bit value v + 32768 in
    // the low mantissa bits of 2^23 is exactly 2^23 + 32768 + v, so the biased
    // difference is the exact float(s1 - s0) and the bias folds out of
    // float(s0) exactly.
    precise float frac = ph - float(b);
    precise float f0 = uintBitsToFloat(0x4B000000u | ((x & 0xFFFFu) ^ 0x8000u));
    precise float f1 = uintBitsToFloat(0x4B000000u | ((x >> 16u) ^ 0x8000u));
    precise float d = f1 - f0;
    precise float m = d * frac;
    precise float u = (f0 - 8421376.0) + m;
    bool relFin = false;
    precise float g = gain;
    uint rr = relRem;
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
    phase = phNext;
    precise float smp = u * kScale;
    if (filt) {
        precise float outv = smp * a0 + z1;
        precise float zin = smp * twoA0 + z2;
        precise float fb1 = b1 * outv;
        precise float nz1 = zin - fb1;
        precise float zin2 = smp * a0;
        precise float fb2 = b2 * outv;
        z1 = nz1;
        z2 = zin2 - fb2;
        smp = outv;
    }
    precise float scaled = smp * g;
    oL = scaled * mixL;
    oR = scaled * mixR;
    if (relFin || (stage == 0u && rr == 0xFFFFFFFFu && g < retireThreshold)) {
        retire = n;
        alive = false;
    }
}

void main() {
    uint t = gl_LocalInvocationID.x;
    uint g = gl_WorkGroupID.x;
    uint idx = g * GROUP + t;
    bool live = idx < uCount;

    precise float phase = 0.0;
    precise float z1 = 0.0;
    precise float z2 = 0.0;
    precise float gain = 0.0;
    uint stage = 3u;
    uint relRem = 0u;
    uint atkRem = 0u;
    uint decRem = 0u;
    uint sampleStart = 0u;
    uint relEnd = 2u;
    uint relLoopS = 0u;
    uint relLoopE = 1u;
    bool filt = false;
    bool loop = false;
    precise float relLoopSF = 0.0;
    precise float relLoopEF = 1.0;
    precise float phaseInc = 0.0;
    precise float mixL = 0.0;
    precise float mixR = 0.0;
    precise float targetGain = 0.0;
    precise float sustainLevel = 0.0;
    precise float attackStep = 0.0;
    precise float decaySlope = 1.0;
    precise float releaseDecay = 1.0;
    precise float a0 = 0.0;
    precise float b1 = 0.0;
    precise float b2 = 0.0;
    uint handle = 0u;
    if (live) {
        handle = elist[idx];
        uint sb = handle * 8u;
        uint r = handle * 20u;
        phase = max(0.0, uintBitsToFloat(state[sb]));
        z1 = uintBitsToFloat(state[sb + 1u]);
        z2 = uintBitsToFloat(state[sb + 2u]);
        gain = uintBitsToFloat(state[sb + 3u]);
        stage = state[sb + 4u];
        atkRem = state[sb + 5u];
        decRem = state[sb + 6u];
        relRem = state[sb + 7u];
        sampleStart = erows[r];
        uint flags = erows[r + 1u];
        filt = (flags & 1u) != 0u;
        loop = (flags & 2u) != 0u;
        relEnd = erows[r + 2u];
        relLoopS = erows[r + 3u];
        relLoopE = erows[r + 4u];
        relLoopSF = uintBitsToFloat(erows[r + 5u]);
        relLoopEF = uintBitsToFloat(erows[r + 6u]);
        phaseInc = uintBitsToFloat(erows[r + 7u]);
        mixL = uintBitsToFloat(erows[r + 8u]);
        mixR = uintBitsToFloat(erows[r + 9u]);
        targetGain = uintBitsToFloat(erows[r + 10u]);
        sustainLevel = uintBitsToFloat(erows[r + 11u]);
        attackStep = uintBitsToFloat(erows[r + 12u]);
        decaySlope = uintBitsToFloat(erows[r + 13u]);
        releaseDecay = uintBitsToFloat(erows[r + 14u]);
        a0 = uintBitsToFloat(erows[r + 15u]);
        b1 = uintBitsToFloat(erows[r + 16u]);
        b2 = uintBitsToFloat(erows[r + 17u]);
    }
    precise float twoA0 = 2.0 * a0;
    const float kScale = 1.0 / 32768.0;
    const float retireThreshold = uintBitsToFloat(uRetireBits);
    const uint kForever = 0xFFFFFFFFu;
    bool alive = live;
    uint retire = 0xFFFFFFu;
    uint row = t / LANES;
    uint lane = t % LANES;
    uint tiles = (uFrames + TILE - 1u) / TILE;
    // Four-frame fast path (released and held-sustained rows): the four
    // frames of the tile are all interior (same sample pair, no wrap, no end of
    // sample), so the loads can be issued up front.  The envelope, filter and
    // retirement still run frame by frame in the generic machine's order.
    const uint interiorLimit = loop ? relLoopE : relEnd;
    for (uint tile = 0u; tile < tiles; ++tile) {
#if defined(SUBGROUP32)
        DECLARE_OV
#endif
        bool fast = alive && (stage == 0u || stage == 3u) &&
                    (tile + 1u) * TILE <= uFrames;
        precise float p1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0;
        if (fast) {
            p1 = phase + phaseInc;
            p2 = p1 + phaseInc;
            p3 = p2 + phaseInc;
            p4 = p3 + phaseInc;
            fast = uint(p3) + 1u < interiorLimit &&
                   (!loop || p4 < relLoopEF);
        }
        if (fast) {
            uint q0 = uint(phase);
            uint q1 = uint(p1);
            uint q2 = uint(p2);
            uint q3 = uint(p3);
            uint i0 = (sampleStart + q0) >> 1u;
            uint i1 = (sampleStart + q1) >> 1u;
            uint i2 = (sampleStart + q2) >> 1u;
            uint i3 = (sampleStart + q3) >> 1u;
            uint wa0 = store[i0], wb0 = store[i0 + 1u];
            uint wa1 = store[i1], wb1 = store[i1 + 1u];
            uint wa2 = store[i2], wb2 = store[i2 + 1u];
            uint wa3 = store[i3], wb3 = store[i3 + 1u];
            precise float oL, oR;
            precise float ph0 = phase;
            uint n0 = tile * TILE;
            EnvFastFrame(wa0, wb0, q0, sampleStart, ph0, p1, n0, stage, filt,
                         a0, b1, b2, twoA0, mixL, mixR, releaseDecay,
                         retireThreshold, phase, gain, relRem, z1, z2, alive,
                         retire, oL, oR);
            EMIT(0u, oL, oR)
            EnvFastFrame(wa1, wb1, q1, sampleStart, p1, p2, n0 + 1u, stage, filt,
                         a0, b1, b2, twoA0, mixL, mixR, releaseDecay,
                         retireThreshold, phase, gain, relRem, z1, z2, alive,
                         retire, oL, oR);
            EMIT(1u, oL, oR)
            EnvFastFrame(wa2, wb2, q2, sampleStart, p2, p3, n0 + 2u, stage, filt,
                         a0, b1, b2, twoA0, mixL, mixR, releaseDecay,
                         retireThreshold, phase, gain, relRem, z1, z2, alive,
                         retire, oL, oR);
            EMIT(2u, oL, oR)
            EnvFastFrame(wa3, wb3, q3, sampleStart, p3, p4, n0 + 3u, stage, filt,
                         a0, b1, b2, twoA0, mixL, mixR, releaseDecay,
                         retireThreshold, phase, gain, relRem, z1, z2, alive,
                         retire, oL, oR);
            EMIT(3u, oL, oR)
        } else
        for (uint k = 0u; k < TILE; ++k) {
            uint n = tile * TILE + k;
            precise float oL = 0.0;
            precise float oR = 0.0;
            if (alive && n < uFrames) {
                uint base = uint(phase);
                bool ended = false;
                if (base + 1u >= relEnd) {
                    if (!loop) {
                        ended = true;
                    } else {
                        phase = relLoopSF;
                        base = relLoopS;
                    }
                }
                if (ended) {
                    retire = n;
                    alive = false;
                } else {
                    uint nextRel = base + 1u;
                    if (loop && nextRel >= relLoopE) nextRel = relLoopS;
                    if (nextRel >= relEnd) nextRel = relEnd - 1u;
                    precise float frac = phase - float(base);
                    int s0 = Sample16(sampleStart + base);
                    int s1 = Sample16(sampleStart + nextRel);
                    precise float d = float(s1 - s0);
                    precise float m = d * frac;
                    precise float u = float(s0) + m;
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
                    phase += phaseInc;
                    if (loop && phase >= relLoopEF) {
                        precise float overflow = phase - relLoopEF;
                        precise float loopLength = relLoopEF - relLoopSF;
                        if (loopLength > 0.0 && overflow >= loopLength)
                            overflow -= floor(overflow / loopLength) * loopLength;
                        phase = relLoopSF + overflow;
                    }
                    precise float smp = u * kScale;
                    if (filt) {
                        precise float outv = smp * a0 + z1;
                        precise float zin = smp * twoA0 + z2;
                        precise float fb1 = b1 * outv;
                        precise float nz1 = zin - fb1;
                        precise float zin2 = smp * a0;
                        precise float fb2 = b2 * outv;
                        z1 = nz1;
                        z2 = zin2 - fb2;
                        smp = outv;
                    }
                    precise float scaled = smp * gain;
                    oL = scaled * mixL;
                    oR = scaled * mixR;
                    if (relFin || (stage == 0u && relRem == kForever &&
                                   gain < retireThreshold)) {
                        retire = n;
                        alive = false;
                    }
                }
            }
            EMIT(k, oL, oR)
        }
#if defined(SUBGROUP32)
        ReduceTile(REDUCE_ARGS, tile, tiles, t, uGroupBase + g);
#else
        barrier();
        {
            precise float acc = 0.0;
            for (uint j = 0u; j < ROWS; ++j)
                acc += sh[row * GROUP + j * LANES + lane];
            sh2[row * LANES + lane] = acc;
        }
        barrier();
        if (lane == 0u) {
            precise float sum = 0.0;
            for (uint j = 0u; j < LANES; ++j)
                sum += sh2[row * LANES + j];
            uint n = tile * TILE + (row >> 1u);
            if (n < uFrames)
                partial[((uGroupBase + g) * uFrames + n) * 2u + (row & 1u)] = sum;
        }
#endif
    }
    if (live) {
        edyn[idx * 2u] = uvec4(floatBitsToUint(phase), floatBitsToUint(z1),
                               floatBitsToUint(z2), floatBitsToUint(gain));
        edyn[idx * 2u + 1u] = uvec4((stage << 24u) | (retire & 0xFFFFFFu),
                                    atkRem, decRem, relRem);
        uint sb = handle * 8u;
        state[sb] = floatBitsToUint(phase);
        state[sb + 1u] = floatBitsToUint(z1);
        state[sb + 2u] = floatBitsToUint(z2);
        state[sb + 3u] = floatBitsToUint(gain);
        state[sb + 4u] = stage;
        state[sb + 5u] = atkRem;
        state[sb + 6u] = decRem;
        state[sb + 7u] = relRem;
    }
}
#endif

#if defined(REDUCE_CS)
layout(std430, binding = 3) readonly buffer PartialBuf { float partial[]; };
layout(std430, binding = 4) writeonly buffer MixBuf { float mixout[]; };
uniform uint uFrames;
uniform uint uGroups;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uFrames * 2u) return;
    precise float acc = 0.0;
    uint stride = uFrames * 2u;
    for (uint g = 0u; g < uGroups; ++g)
        acc += partial[g * stride + i];
    mixout[i] = acc;
}
#endif

#if defined(SCATTER_CS)
layout(std430, binding = 1) writeonly buffer RowBuf { uint rows[]; };
layout(std430, binding = 6) readonly buffer UpBuf { uint upload[]; };
layout(std430, binding = 11) writeonly buffer StateBuf { uint state[]; };
uniform uint uCount;
uniform uint uBase;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uCount) return;
    uint b = (uBase + i) * 16u;
    uint h = upload[b];
    for (uint w = 0u; w < 12u; ++w)
        rows[h * 12u + w] = upload[b + 1u + w];
    for (uint w = 0u; w < 3u; ++w)
        state[h * 8u + w] = upload[b + 13u + w];
}
#endif

#if defined(ENV_SCATTER_CS)
layout(std430, binding = 7) writeonly buffer EnvRowBuf { uint erows[]; };
layout(std430, binding = 10) readonly buffer EnvUpBuf { uint eupload[]; };
layout(std430, binding = 11) writeonly buffer StateBuf { uint state[]; };
uniform uint uCount;
uniform uint uBase;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uCount) return;
    uint b = (uBase + i) * 32u;
    uint h = eupload[b];
    for (uint w = 0u; w < 20u; ++w)
        erows[h * 20u + w] = eupload[b + 1u + w];
    for (uint w = 0u; w < 8u; ++w)
        state[h * 8u + w] = eupload[b + 21u + w];
}
#endif
)GLSL";

} // namespace gpu
} // namespace svms

#endif
