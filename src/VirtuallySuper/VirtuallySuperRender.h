#ifndef VIRTUALLYSUPER_RENDER_H
#define VIRTUALLYSUPER_RENDER_H

#include "VirtuallySuperDensity.h"
#include "VirtuallySuperExact.h"
#include "VirtuallySuperGrouped.h"
#include "VirtuallySuperRenderSIMD.h"

namespace virtuallysuper {

class RenderSystem {
public:
  RenderSystem();

  void Reset();
  void ResetBlockStats();
  void RenderBlock(ExactSystem &exact, const GroupedSystem &grouped,
                   const DensitySystem &density, float *output, int numFrames,
                   int sampleRate);
  const RenderStats &GetStats() const;

private:
  // Optimized render paths
  void RenderExactTile(ExactSystem &exact, float *tileOutput, uint32_t frames);
  void RenderExactTileSIMD(ExactSystem &exact, float *tileOutput, uint32_t frames);
  void RenderExactVoiceBatch(ExactSystem &exact, uint32_t *voiceHandles,
                             uint32_t voiceCount, float *tileOutput,
                             uint32_t frames, bool useSIMD);
  
  void RenderGroupedTile(const GroupedSystem &grouped, float *tileOutput,
                         uint32_t frames);
  void RenderGroupedTileSIMD(const GroupedSystem &grouped, float *tileOutput,
                             uint32_t frames);
  
  void RenderDensityTile(const DensitySystem &density, float *tileOutput,
                         uint32_t frames, uint32_t tileStart);
  
  // Helper functions for optimized rendering
  static void MixVoiceSamples(float *output, const float *samples,
                              float leftGain, float rightGain, uint32_t frames);
  static void ApplyEnvelope(float *samples, float *gains,
                            const float *attackSteps, uint16_t *attackRemaining,
                            float targetGain, uint32_t frames);

  RenderStats stats_;
  VoiceSoABuffer voiceBuffer_;
  bool simdEnabled_;
};

} // namespace virtuallysuper

#endif
