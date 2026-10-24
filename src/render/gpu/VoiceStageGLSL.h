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
#if defined(VOICE_CS)
layout(local_size_x = GROUP) in;
#else
layout(local_size_x = 64) in;
#endif

#if defined(VOICE_CS)
#if TILE != 4
#error TILE must be 4 (the fast path is written out for four frames)
#endif
#define ROWS (2u * TILE)
#define LANES (GROUP / ROWS)
layout(std430, binding = 0) readonly buffer StoreBuf { uint store[]; };
layout(std430, binding = 1) readonly buffer RowBuf { uint rows[]; };
layout(std430, binding = 2) readonly buffer ListBuf { uvec4 listv[]; };
layout(std430, binding = 3) writeonly buffer PartialBuf { float partial[]; };
layout(std430, binding = 5) writeonly buffer DynBuf { vec4 dynv[]; };
uniform uint uCount;
uniform uint uFrames;

// ROWS rows = TILE frames x (left, right); one column per voice of the group.
shared float sh[ROWS * GROUP];
shared float sh2[ROWS * LANES];
uint StoreWord(uint wi) { return store[wi]; }

// Signed 16-bit sample `i` of the store (two samples per 32-bit word).
int Sample16(uint i) {
    return bitfieldExtract(int(StoreWord(i >> 1u)), int((i & 1u) << 4u), 16);
}

// Fast path: one interior frame of a running span (no clamp, no wrap).
#define FAST_POS(I, F) { uint b_ = uint(phase); I = sampleStart + b_; F = phase - float(b_); phase += phaseInc; }
// The two samples at positions i, i+1 as the halves of one 32-bit value,
// without a branch: shift the 64-bit window (wa, wb) right by 16 * (i & 1).
#define FAST_LOAD(I) uint wa_ = store[(I) >> 1u]; uint wb_ = store[((I) >> 1u) + 1u];
#define FAST_PAIR(I) uint p_ = ((I) & 1u) << 4u; FAST_LOAD(I) uint x_ = (wa_ >> p_) | ((wb_ << (31u - p_)) << 1u); int s0_ = (int(x_) << 16) >> 16; int s1_ = int(x_) >> 16; precise float d_ = float(s1_ - s0_); precise float m_ = d_ * f_; precise float u_ = float(s0_) + m_;
#define FAST_OUT_PLAIN(K, I, F) { precise float f_ = (F); FAST_PAIR(I) sh[(2u * (K)) * GROUP + t] = u_ * gainLs; sh[(2u * (K) + 1u) * GROUP + t] = u_ * gainRs; }
#define FAST_OUT_FILT(K, I, F) { precise float f_ = (F); FAST_PAIR(I) precise float smp_ = u_ * kScale; precise float outv_ = smp_ * a0 + z1; precise float zin_ = smp_ * twoA0 + z2; precise float fb1_ = b1 * outv_; precise float nz1_ = zin_ - fb1_; precise float zin2_ = smp_ * a0; precise float fb2_ = b2 * outv_; z1 = nz1_; z2 = zin2_ - fb2_; sh[(2u * (K)) * GROUP + t] = outv_ * gainL; sh[(2u * (K) + 1u) * GROUP + t] = outv_ * gainR; }

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
    if (live) {
        uvec4 e = listv[idx];
        uint b = e.x * 12u;
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
        phase = max(0.0, uintBitsToFloat(e.y));
        z1 = uintBitsToFloat(e.z);
        z2 = uintBitsToFloat(e.w);
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
                sh[(2u * k) * GROUP + t] = oL;
                sh[(2u * k + 1u) * GROUP + t] = oR;
                cValid = nValid; cFirst = nFirst; cFrac = nFrac; cEdge = nEdge;
                cA = nA; cB = nB;
            }
        }
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
    }
    if (live)
        dynv[idx] = vec4(phase, z1, z2, 0.0);
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
uniform uint uCount;
uniform uint uBase;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uCount) return;
    uint b = (uBase + i) * 16u;
    uint h = upload[b];
    for (uint w = 0u; w < 12u; ++w)
        rows[h * 12u + w] = upload[b + 1u + w];
}
#endif
)GLSL";

} // namespace gpu
} // namespace svms

#endif
