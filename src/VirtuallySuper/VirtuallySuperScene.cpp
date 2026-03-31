#include "VirtuallySuperScene.h"

namespace virtuallysuper {

SceneCompiler::SceneCompiler() : stats_() {}

void SceneCompiler::Reset() { stats_ = SceneStats(); }

void SceneCompiler::BeginWindow() { stats_ = SceneStats(); }

SceneAction SceneCompiler::CompileEvent(const NormalizedEvent &event,
                                        const ExactStats &exactStats,
                                        PressureLevel pressureLevel) {
  SceneAction action;
  action.event = event;
  action.importanceScore = ScoreEvent(event, exactStats, pressureLevel);

  switch (event.kind) {
  case EventKind::NoteOn: {
    const bool strongAttack = action.importanceScore >= 208u;
    const bool mediumAttack = action.importanceScore >= 128u;

    switch (pressureLevel) {
    case PressureLevel::Normal:
      action.kind = SceneActionKind::SpawnExactVoice;
      action.protectedAttack = 1;
      ++stats_.exactActions;
      ++stats_.protectedAttacks;
      break;
    case PressureLevel::Soft:
      action.kind = SceneActionKind::SpawnExactVoice;
      action.protectedAttack = 1;
      action.observeGrouped = mediumAttack ? 0 : 1;
      ++stats_.exactActions;
      ++stats_.protectedAttacks;
      if (action.observeGrouped != 0)
        ++stats_.groupedObservations;
      break;
    case PressureLevel::Hard:
      if (strongAttack) {
        action.kind = SceneActionKind::SpawnExactVoice;
        action.protectedAttack = 1;
        ++stats_.exactActions;
        ++stats_.protectedAttacks;
      } else if (mediumAttack) {
        action.observeGrouped = 1;
        ++stats_.groupedObservations;
        ++stats_.groupedOnlyActions;
      } else {
        action.observeDensity = 1;
        ++stats_.densityObservations;
        ++stats_.densityOnlyActions;
      }
      break;
    case PressureLevel::Panic:
      ++stats_.panicDecisions;
      if (strongAttack) {
        action.kind = SceneActionKind::SpawnExactVoice;
        action.protectedAttack = 1;
        ++stats_.exactActions;
        ++stats_.protectedAttacks;
      } else if (action.importanceScore >= 96u) {
        action.observeGrouped = 1;
        ++stats_.groupedObservations;
        ++stats_.groupedOnlyActions;
      } else {
        action.observeDensity = 1;
        ++stats_.densityObservations;
        ++stats_.densityOnlyActions;
      }
      break;
    }
    break;
  }
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
                                   const ExactStats &exactStats,
                                   PressureLevel pressureLevel) const {
  uint16_t score = 0;
  if (event.kind == EventKind::NoteOn)
    score = (uint16_t)(event.velocity * 2u);
  else if (event.kind == EventKind::NoteOff)
    score = 32u;
  else if (event.kind == EventKind::Reset)
    score = 255u;

  if (exactStats.activeVoices < 32)
    score = (uint16_t)(score + 16u);

  if (pressureLevel == PressureLevel::Soft)
    score = (uint16_t)(score + 8u);
  else if (pressureLevel == PressureLevel::Hard)
    score = (uint16_t)(score + 16u);
  else if (pressureLevel == PressureLevel::Panic)
    score = (uint16_t)(score + 24u);
  return score;
}

} // namespace virtuallysuper
