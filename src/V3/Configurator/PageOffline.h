#ifndef SVMS_CONFIGURATOR_PAGEOFFLINE_H
#define SVMS_CONFIGURATOR_PAGEOFFLINE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace svms::cfg {

class ConfigDocument;

// UI-side supervisor for the standalone svms_v3_render process. Keeping the
// renderer out of the Configurator process gives the CLI and GUI exactly one
// MIDI/parser/WAV implementation and isolates large offline jobs from ImGui.
class OfflineRendererPage {
public:
    enum class State {
        Idle,
        Starting,
        Loading,
        Preparing,
        Rendering,
        Tail,
        Cancelling,
        Complete,
        Cancelled,
        Error
    };

    OfflineRendererPage() = default;
    ~OfflineRendererPage();

    OfflineRendererPage(const OfflineRendererPage&) = delete;
    OfflineRendererPage& operator=(const OfflineRendererPage&) = delete;

    void Draw(ConfigDocument& document, HWND owner);
    void Shutdown();

private:
    struct Snapshot {
        State state = State::Idle;
        double loadProgress = 0.0;
        double loadElapsed = 0.0;
        double loadEta = 0.0;
        double renderProgress = 0.0;
        double elapsed = 0.0;
        double renderedSeconds = 0.0;
        double totalSeconds = 0.0;
        double speed = 0.0;
        double eta = 0.0;
        uint32_t activeVoices = 0;
        uint32_t peakVoices = 0;
        uint64_t steals = 0;
        uint64_t events = 0;
        std::string message = "Select a MIDI file, SoundFont, and output WAV.";
        std::string detail;
    };

    void EnsureInitialPaths(const ConfigDocument& document);
    void BrowseMidi(HWND owner);
    void BrowseSoundFont(HWND owner);
    void BrowseOutput(HWND owner);
    void Start(const ConfigDocument& document);
    void Cancel();
    void JoinFinishedWorker();
    void ReadProcessOutput(HANDLE readPipe, HANDLE process);
    void HandleProtocolLine(const std::string& line);

    static std::wstring ExecutableDirectory();
    static std::wstring QuoteArgument(const std::wstring& value);

    std::wstring midiPath_;
    std::wstring soundFontPath_;
    std::wstring outputPath_;
    std::wstring lastMidiDirectory_;
    std::wstring lastSoundFontDirectory_;
    std::wstring lastOutputDirectory_;
    bool pathsInitialized_ = false;
    bool outputPathAutomatic_ = true;

    std::mutex stateMutex_;
    Snapshot snapshot_;
    HANDLE process_ = nullptr;
    HANDLE cancelEvent_ = nullptr;
    std::thread worker_;
    std::atomic<bool> workerRunning_{false};
    bool shutdown_ = false;
};

} // namespace svms::cfg

#endif
