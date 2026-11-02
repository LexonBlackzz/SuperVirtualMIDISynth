#ifndef SVMS_CONFIGURATOR_STAGEAUDITION_H
#define SVMS_CONFIGURATOR_STAGEAUDITION_H

#include <memory>
#include <mutex>
#include <string>

namespace svms::cfg {

struct ConfigValues;

// Live listening tests for the 3D Stage page. A test sound is rendered through
// the room response the engine would build from the page's current (unsaved)
// settings and played on the default output device until Stop(). While it
// plays, Update() feeds the page's edits in: moving a channel or the listener
// glides the sound, changing the room, its surfaces or furniture rebuilds the
// tail.
class StageAudition {
public:
    enum class Mode {
        Channel,   // the selected channel plays an arpeggio
        Music,     // the loop plays through the selected channel
        Tour,      // each channel plays one note in turn, 1 to 16
        Around,    // the loop circles the listener once per pass
        Walk,      // the selected channel circles the listener
        Surround,  // six surround tracks on channels 1 to 6
    };

    StageAudition();
    ~StageAudition();
    void Start(Mode mode, const ConfigValues& values, int selected);
    // Call every frame; does nothing while idle.
    void Update(const ConfigValues& values, int selected);
    // Hold to hear the same sound without the room and its reflections.
    void SetDry(bool dry);
    void Stop();
    bool Busy() const;
    std::string Status() const;
    static bool Supported();

private:
    struct Session;
    void SetStatus(const std::string& text);

    std::unique_ptr<Session> session_;
    mutable std::mutex lock_;
    std::string status_ = "Stopped";
};

} // namespace svms::cfg

#endif
