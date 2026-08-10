#ifndef SVMS_DIAG_WINDOW_H
#define SVMS_DIAG_WINDOW_H

#include "SVMSTypes.h"

namespace svms {

void DiagWindow_Create(bool showWindow, bool debugOutput);
void DiagWindow_Destroy();
void DiagWindow_Update(uint32_t activeVoices, uint32_t maxVoices,
                        uint32_t releasingVoices, uint32_t sustainHeldVoices,
                        uint32_t voiceSteals,
                        float cpuPercent, uint32_t decimationStep,
                        float callbackP95, float callbackP99,
                        float callbackP999, uint64_t overBudgetCallbacks,
                        uint32_t maxConsecutiveOverBudget,
                        uint32_t retired, uint32_t retiredImmediate,
                        const LiveSF2Telemetry& sf2);

} // namespace svms

#endif
