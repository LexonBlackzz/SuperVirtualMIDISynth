#include "configurator/PageExperimental.h"
#include "configurator/ConfigDocument.h"
#include "configurator/DriverLink.h"
#include "configurator/ModernWidgets.h"
#include "configurator/Theme.h"
#include "configurator/Widgets.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace svms::cfg {

namespace {

// Budget steps, percent of the voice limit per block (0 = off).  A value that
// came from the config file and is not a step shows the nearest one; nothing
// changes until a step is clicked.
constexpr uint32_t kBudgetSteps[] = {0u, 100u, 200u, 400u, 800u};

int NearestBudgetStep(uint32_t percent) {
    int best = 0;
    uint32_t bestDistance = UINT32_MAX;
    for (int i = 0; i < static_cast<int>(std::size(kBudgetSteps)); ++i) {
        const uint32_t d = percent > kBudgetSteps[i] ? percent - kBudgetSteps[i]
                                                     : kBudgetSteps[i] - percent;
        if (d < bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

// A restart-only toggle row (the ↻ mark comes from the registry).
void ToggleRow(ConfigDocument& doc, const char* anchor, const char* title, const char* desc, bool* value,
               const ui::ChipDef* chip) {
    const float s = ui::Scale();
    ui::BeginRow(anchor, title, desc, ImVec2(46.0f * s, 28.0f * s), chip, chip ? 1 : 0);
    if (ui::Toggle(anchor, value)) doc.MarkDirty();
    ui::EndRow();
}

} // namespace

void DrawExperimentalPage(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const ui::ChipDef changes = {"Changes the sound", ui::ChipKind::Bad};
    const ui::ChipDef same = {"Same sound", ui::ChipKind::Ok};
    ui::BeginCard("##experimental");
    {
        static const char* labels[5] = {"Off", "100%", "200%", "400%", "800%"};
        ui::BeginRow("bulkAdmission", "Bulk note admission",
                     "In very dense blocks, only the loudest notes start. Lower is lighter and darker.",
                     ui::SegmentedSize(labels, 5), &changes, 1);
        int idx = NearestBudgetStep(w.bulkAdmissionPercent);
        if (ui::Segmented("##bulkadmission", &idx, labels, 5)) {
            w.bulkAdmissionPercent = kBudgetSteps[idx];
            doc.MarkDirty();
            SendLiveCommand(SVMS_COMMAND_SET_BULK_ADMISSION, w.bulkAdmissionPercent);
        }
        ui::EndRow();
    }
    ToggleRow(doc, "sampledSteal", "Sampled voice stealing",
              "Steals from a rotating sample of voices. Cleaner when dense, slower. From SSV2 by Savrien.",
              &w.sampledSteal, &changes);
    ToggleRow(doc, "dropZeroLife", "Drop instant-steal blips", "Skips voices stolen the moment they start.",
              &w.dropZeroLifeVictims, &changes);
    {
        static const char* labels[6] = {"Off", "Auto", "OpenGL", "Vulkan", "D3D11", "CUDA"};
        int idx = static_cast<int>((std::min)(5u, w.gpuApi));
        ui::BeginRow("gpuStage", "GPU rendering", "Sustained voices render on the GPU. No D3D11 yet.",
                     ui::SegmentedSize(labels, 6));
        if (ui::Segmented("##gpustage", &idx, labels, 6)) {
            w.gpuApi = static_cast<uint32_t>(idx);
            doc.MarkDirty();
        }
        ui::EndRow();
    }
    ToggleRow(doc, "launchStaging", "Staged note launches (always)", "Lighter planning on huge files.",
              &w.launchStaging, &same);
    ToggleRow(doc, "launchStagingAuto", "Staged launches for large pools", "Turns staged launches on at 1,024 voices and up.",
              &w.launchStagingAuto, &same);
    ui::EndCard();
}

} // namespace svms::cfg
