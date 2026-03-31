#ifndef VIRTUALLYSUPER_SCENE_H
#define VIRTUALLYSUPER_SCENE_H

#include "VirtuallySuperTypes.h"

namespace virtuallysuper {

class SceneCompiler {
public:
  SceneCompiler();

  void Reset();
  void BeginWindow();
  SceneAction CompileEvent(const NormalizedEvent &event,
                           const ExactStats &exactStats,
                           PressureLevel pressureLevel);
  const SceneStats &GetStats() const;

private:
  uint16_t ScoreEvent(const NormalizedEvent &event,
                      const ExactStats &exactStats,
                      PressureLevel pressureLevel) const;

  SceneStats stats_;
};

} // namespace virtuallysuper

#endif
