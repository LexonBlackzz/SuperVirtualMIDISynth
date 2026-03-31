#ifndef VIRTUALLYSUPER_RENDER_H
#define VIRTUALLYSUPER_RENDER_H

#include "VirtuallySuperDensity.h"
#include "VirtuallySuperExact.h"
#include "VirtuallySuperGrouped.h"

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
  void RenderExactTile(ExactSystem &exact, float *tileOutput, uint32_t frames);
  void RenderGroupedTile(const GroupedSystem &grouped, float *tileOutput,
                         uint32_t frames);
  void RenderDensityTile(const DensitySystem &density, float *tileOutput,
                         uint32_t frames, uint32_t tileStart);

  RenderStats stats_;
};

} // namespace virtuallysuper

#endif
