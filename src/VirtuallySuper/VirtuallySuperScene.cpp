#include "VirtuallySuperScene.h"

namespace virtuallysuper {

SceneCompiler::SceneCompiler() : stats_() {}

void SceneCompiler::Reset() { stats_ = SceneStats(); }

void SceneCompiler::BeginWindow() { stats_ = SceneStats(); }

SceneAction SceneCompiler::CompileEvent(const NormalizedEvent &event,
                                        const ExactStats &exactStats) {
  SceneAction action;
  action.event = event;
  action.importanceScore = ScoreEvent(event, exactStats);

  switch (event.kind) {
  case EventKind::NoteOn:
    action.kind = SceneActionKind::SpawnExactVoice;
    action.observeGrouped = 1;
    action.observeDensity = 1;
    action.protectedAttack = 1;
    ++stats_.exactActions;
    ++stats_.groupedObservations;
    ++stats_.densityObservations;
    ++stats_.protectedAttacks;
    break;
  case EventKind::NoteOff:
    action.kind = SceneActionKind::ReleaseExactVoice;
    ++stats_.exactActions;
    break;
  case EventKind::Reset:
    action.kind = SceneActionKind::ResetScene;
    ++stats_.resetActions;
    break;
  default:
    action.kind = SceneActionKind::None;
    break;
  }

  return action;
}

const SceneStats &SceneCompiler::GetStats() const { return stats_; }

uint16_t SceneCompiler::ScoreEvent(const NormalizedEvent &event,
                                   const ExactStats &exactStats) const {
  uint16_t score = 0;
  if (event.kind == EventKind::NoteOn)
    score = (uint16_t)(event.velocity * 2u);
  else if (event.kind == EventKind::NoteOff)
    score = 32u;
  else if (event.kind == EventKind::Reset)
    score = 255u;

  if (exactStats.activeVoices < 32)
    score = (uint16_t)(score + 16u);
  return score;
}

} // namespace virtuallysuper
