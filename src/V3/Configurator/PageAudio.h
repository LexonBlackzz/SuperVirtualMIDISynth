#ifndef SVMS_CONFIGURATOR_PAGEAUDIO_H
#define SVMS_CONFIGURATOR_PAGEAUDIO_H

struct ConfigDocument;
struct EasterEggState;

namespace svms::cfg {

// Output = device/rate/buffer/backend; SoundFont = the SoundFont stack.
enum class AudioView { Both, Output, SoundFont };

void DrawAudioPage(ConfigDocument& doc, const EasterEggState& easterEggs,
                   AudioView view = AudioView::Both);

} // namespace svms::cfg

#endif
