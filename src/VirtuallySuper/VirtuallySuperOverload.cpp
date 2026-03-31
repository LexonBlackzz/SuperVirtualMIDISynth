#include "VirtuallySuperOverload.h"

namespace virtuallysuper {

namespace {

static uint32_t MaxValue(uint32_t a, uint32_t b) { return a > b ? a : b; }

} // namespace

OverloadController::OverloadController() : config_(), initialized_(false) {}

bool OverloadController::Initialize(const OverloadConfig &config,
                                    const ExactConfig &exactConfig,
                                    const SchedulerConfig &schedulerConfig) {
  config_ = config;

  const uint32_t exactCapacity =
      exactConfig.maxVoices > 0 ? exactConfig.maxVoices : kDefaultExactVoiceCapacity;
  const uint32_t schedulerCapacity = schedulerConfig.scheduledCapacity > 0
                                         ? schedulerConfig.scheduledCapacity
                                         : kDefaultScheduledCapacity;

  if (config_.softExactVoiceThreshold == 0)
    config_.softExactVoiceThreshold = MaxValue(16u, exactCapacity * 3u / 5u);
  if (config_.hardExactVoiceThreshold == 0)
    config_.hardExactVoiceThreshold = MaxValue(
        config_.softExactVoiceThreshold + 1u, exactCapacity * 4u / 5u);
  if (config_.panicExactVoiceThreshold == 0)
    config_.panicExactVoiceThreshold = MaxValue(
        config_.hardExactVoiceThreshold + 1u, exactCapacity * 9u / 10u);

  if (config_.softSchedulerQueueThreshold == 0)
    config_.softSchedulerQueueThreshold = MaxValue(64u, schedulerCapacity / 8u);
  if (config_.hardSchedulerQueueThreshold == 0)
    config_.hardSchedulerQueueThreshold =
        MaxValue(config_.softSchedulerQueueThreshold + 1u, schedulerCapacity / 4u);
  if (config_.panicSchedulerQueueThreshold == 0)
    config_.panicSchedulerQueueThreshold = MaxValue(
        config_.hardSchedulerQueueThreshold + 1u, schedulerCapacity / 2u);

  initialized_ = true;
  return true;
}

void OverloadController::Reset() {}

PressureLevel OverloadController::Evaluate(uint32_t currentScheduledCount,
                                           const SchedulerStats &schedulerStats,
                                           const ExactStats &exactStats,
                                           const GroupedStats &groupedStats,
                                           const DensityStats &densityStats) const {
  if (!initialized_)
    return PressureLevel::Normal;

  (void)schedulerStats;

  const uint32_t voiceEquivalent =
      exactStats.activeVoices + groupedStats.noteOnsAccumulated +
      densityStats.noteOnsAccumulated;

  if (exactStats.activeVoices >= config_.panicExactVoiceThreshold ||
      currentScheduledCount >= config_.panicSchedulerQueueThreshold ||
      voiceEquivalent >= config_.panicExactVoiceThreshold * 3u) {
    return PressureLevel::Panic;
  }

  if (exactStats.activeVoices >= config_.hardExactVoiceThreshold ||
      currentScheduledCount >= config_.hardSchedulerQueueThreshold ||
      voiceEquivalent >= config_.hardExactVoiceThreshold * 3u) {
    return PressureLevel::Hard;
  }

  if (exactStats.activeVoices >= config_.softExactVoiceThreshold ||
      currentScheduledCount >= config_.softSchedulerQueueThreshold ||
      voiceEquivalent >= config_.softExactVoiceThreshold * 2u) {
    return PressureLevel::Soft;
  }

  return PressureLevel::Normal;
}

} // namespace virtuallysuper
