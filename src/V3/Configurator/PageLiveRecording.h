#ifndef SVMS_CONFIGURATOR_PAGE_LIVE_RECORDING_H
#define SVMS_CONFIGURATOR_PAGE_LIVE_RECORDING_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace svms {
class RuntimeLinkClient;
}

namespace svms::cfg {

class LiveRecordingPage {
public:
    void Draw(svms::RuntimeLinkClient* client, bool connected, HWND owner);

private:
    void EnsureDefaultPath();
    void Browse(HWND owner);
    void Start(svms::RuntimeLinkClient& client);
    void Stop(svms::RuntimeLinkClient& client);
    void Query(svms::RuntimeLinkClient& client, bool force);

    std::wstring outputPath_;
    std::wstring lastDirectory_;
    bool pathInitialized_ = false;
    uint32_t state_ = 0u;
    uint32_t sampleRate_ = 0u;
    uint64_t framesWritten_ = 0u;
    uint64_t droppedFrames_ = 0u;
    uint32_t errorCode_ = 0u;
    float queryTimer_ = 0.0f;
    std::string status_ = "Connect to a running V3 driver to record.";
    bool statusError_ = false;
};

} // namespace svms::cfg

#endif // SVMS_CONFIGURATOR_PAGE_LIVE_RECORDING_H
