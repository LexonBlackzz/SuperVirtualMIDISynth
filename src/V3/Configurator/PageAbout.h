#ifndef SVMS_CONFIGURATOR_PAGEABOUT_H
#define SVMS_CONFIGURATOR_PAGEABOUT_H

struct ConfigDocument;

namespace svms::cfg {

struct FramePacingStats {
    int frameCount = 0;
    float avgMs = 0.0f;
    float p95Ms = 0.0f;
    float worstMs = 0.0f;
    int histogram[10] = {};  // 4 ms bins over 0..40 ms
};

void DrawAboutPage(ConfigDocument& doc, bool vsyncEnabled,
                   const FramePacingStats& pacing);

} // namespace svms::cfg

#endif
