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
  void RenderBlock(ExactSystem &exact, const GroupedSystem &grouped,
                   const DensitySystem &density, float *output, int numFrames,
                   int sampleRate);

private:
  void RenderExactTile(ExactSystem &exact, uint32_t tileStart, uint32_t frames,
                       int sampleRate);
  void RenderGroupedTile(const GroupedSystem &grouped, uint32_t tileStart,
                         uint32_t frames, int sampleRate);
  void RenderDensityTile(const DensitySystem &density, uint32_t tileStart,
                         uint32_t frames);
  void ClearTileBuffer(uint32_t frames);
  void CopyTileToOutput(float *output, uint32_t tileStart, uint32_t frames);

  float tileBuffer_[kMaxRenderTileFrames * 2];
};

} // namespace virtuallysuper

#endif
