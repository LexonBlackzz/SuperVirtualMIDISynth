#ifndef SVMS_CONFIGURATOR_STAGEAUDITION_H
#define SVMS_CONFIGURATOR_STAGEAUDITION_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace svms::cfg {

struct ConfigValues;

// Listening tests for the 3D Stage page. A short test piece is synthesised,
// run through the same room response the engine builds from the page's current
// (unsaved) settings, and played on the default output device. Rendering takes
// a few seconds on a background thread; Stop() ends it at any time.
class StageAudition {
public:
    enum class Mode {
        Channel,   // the selected channel plays an arpeggio
        Music,     // all unmuted channels play a short loop
        Tour,      // each channel plays one note in turn, 1 to 16
        Around,    // the music circles the listener once
        Walk,      // the selected channel circles the listener once
        Dry,       // the selected channel without room or reflections
        Surround,  // six surround tracks on channels 1 to 6
    };

    ~StageAudition() { Stop(); }
    void Start(Mode mode, const ConfigValues& values, int selected);
    void Stop();
    bool Busy() const;
    std::string Status() const;
    static bool Supported();

private:
    void Work(Mode mode, const ConfigValues& values, int selected);
    void SetStatus(const std::string& text);

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> busy_{false};
    mutable std::mutex lock_;
    std::string status_ = "Stopped";
};

} // namespace svms::cfg

#endif
