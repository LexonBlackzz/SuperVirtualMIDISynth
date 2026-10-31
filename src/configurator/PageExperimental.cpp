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

void PlaybackCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    ui::BeginCard("##expplay", "While it plays",
                  "Applied at once, no restart. Use these to hear what they do.",
                  0.0f, "expplay");
    {
        static const char* labels[5] = {"Off", "100%", "200%", "400%", "800%"};
        ui::BeginRow("bulkAdmission", "Bulk note admission",
                     "When a block carries more new notes than this share of the voice limit, only the loudest are started (by velocity, ties to the later note) and the rest never launch. The notes that stay keep their exact timing. It makes extreme black MIDIs far lighter, but it changes the sound: the many short, quickly stolen notes are where the bright noise lives, so lower budgets sound darker and fuller in the lows. 100% is the strongest, 800% is close to unchanged, Off keeps every note.",
                     ui::SegmentedSize(labels, 5));
        int idx = NearestBudgetStep(w.bulkAdmissionPercent);
        if (ui::Segmented("##bulkadmission", &idx, labels, 5)) {
            w.bulkAdmissionPercent = kBudgetSteps[idx];
            doc.MarkDirty();
            SendLiveCommand(SVMS_COMMAND_SET_BULK_ADMISSION,
                            w.bulkAdmissionPercent);
        }
        ui::EndRow();
    }
    ui::EndCard();
}

void EngineCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    ui::BeginCard("##expengine", "Needs a restart",
                  "Read when the engine starts.", 0.0f, "expengine");
    {
        ui::BeginRow("sampledSteal", "Sampled voice stealing",
                     "When the voice pool is full, a new note replaces the cheapest voice in a rotating sample of the pool instead of the single quietest one, and is dropped if nothing in the sample is quiet enough. Keeps more fidelity in dense black MIDIs at higher voice limits; slower than the default. Based on the voice stealing in SSV2 by Savrien.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##sampledsteal", &w.sampledSteal)) doc.MarkDirty();
        ui::EndRow();
    }
    {
        ui::BeginRow("dropZeroLife", "Drop instant-steal blips",
                     "A voice stolen on the same frame it started never plays a sample. This skips the short fade-out it would leave and lets those launches skip most of their work, which makes extreme black MIDIs far lighter on one shard. May change the sound slightly.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##dropzerolife", &w.dropZeroLifeVictims)) doc.MarkDirty();
        ui::EndRow();
    }
    {
        static const char* labels[6] = {"Off", "Auto", "OpenGL", "Vulkan", "D3D11", "CUDA"};
        int idx = static_cast<int>((std::min)(5u, w.gpuApi));
        ui::BeginRow("gpuStage", "GPU rendering",
                     "Renders steady sustained voices on the graphics card so a CPU can reach far higher voice counts. Falls back to the CPU on any problem. CUDA needs an NVIDIA card; OpenGL needs 4.3; Vulkan needs any Vulkan 1.1 card (D3D11 is not built yet); Auto tries CUDA, then OpenGL.",
                     ui::SegmentedSize(labels, 6));
        if (ui::Segmented("##gpustage", &idx, labels, 6)) {
            w.gpuApi = static_cast<uint32_t>(idx);
            doc.MarkDirty();
        }
        ui::EndRow();
    }
    {
        ui::BeginRow("launchStaging", "Staged note launches (always)",
                     "Writes a stolen voice's replacement into one compact record and lets the render threads finish the copy, so the single planning thread does less work on extreme black MIDIs. The sound is bit-identical. This forces it on at any pool size.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##launchstaging", &w.launchStaging)) doc.MarkDirty();
        ui::EndRow();
    }
    {
        ui::BeginRow("launchStagingAuto", "Staged launches for large pools",
                     "Turns staged note launches on by itself when the voice pool is 1024 voices or more, where it speeds up extreme black MIDIs by 25 to 30 percent. Smaller pools keep the direct path. The sound is bit-identical either way.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##launchstagingauto", &w.launchStagingAuto)) doc.MarkDirty();
        ui::EndRow();
    }
    ui::EndCard();
}

} // namespace

void DrawExperimentalPage(ConfigDocument& doc) {
    PlaybackCard(doc);
    EngineCard(doc);
}

} // namespace svms::cfg
