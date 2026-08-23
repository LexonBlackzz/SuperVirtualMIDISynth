#include "SVMSMPSCQueue.h"
#include "SVMSStandaloneSynth.h"
#include "SVMSNativeOffline.h"
#include "SVMSBuildInfo.h"
#include "include/svmsapi.h"

#include <alsa/asoundlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <locale>
#include <mutex>
#include <new>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr uint32_t kDefaultEventCapacity = 393216u;

uint64_t MonotonicNanoseconds() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &value);
    return uint64_t(value.tv_sec) * 1000000000ull + uint64_t(value.tv_nsec);
}

bool FileExists(const std::string& path) {
    struct stat status{};
    return !path.empty() && stat(path.c_str(), &status) == 0 &&
           S_ISREG(status.st_mode);
}

bool IsAbsolute(const std::string& path) {
    return !path.empty() && path[0] == '/';
}

std::string DirectoryName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0u) return "/";
    return path.substr(0u, slash);
}

std::string JoinPath(const std::string& directory, const std::string& name) {
    if (name.empty() || IsAbsolute(name)) return name;
    if (directory.empty() || directory == ".") return name;
    return directory.back() == '/' ? directory + name
                                   : directory + "/" + name;
}

std::string CurrentDirectory() {
    std::vector<char> buffer(4096u);
    return getcwd(buffer.data(), buffer.size()) ? buffer.data() : ".";
}

std::string LibraryDirectory() {
    Dl_info information{};
    if (dladdr(reinterpret_cast<void*>(&LibraryDirectory), &information) &&
        information.dli_fname) {
        char resolved[PATH_MAX]{};
        if (realpath(information.dli_fname, resolved))
            return DirectoryName(resolved);
        return DirectoryName(information.dli_fname);
    }
    return CurrentDirectory();
}

std::wstring Utf8ToWide(const std::string& value) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.from_bytes(value);
}

struct RuntimeOptions {
    svms::StandaloneSynthConfig synth;
    std::string pcmDevice = "default";
    uint32_t bufferFrames = 2048u;
    uint32_t eventCapacity = kDefaultEventCapacity;
    std::string configPath;
};

template <typename T>
void ReadNumber(const json& object, const char* key, T minimum, T maximum,
                T& destination) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) return;
    try {
        const T value = found->get<T>();
        if (value >= minimum && value <= maximum) destination = value;
    } catch (...) {}
}

void ReadBoolean(const json& object, const char* key, bool& destination) {
    const auto found = object.find(key);
    if (found != object.end() && found->is_boolean())
        destination = found->get<bool>();
}

std::string LocateConfig(const std::string& libraryDirectory) {
    if (const char* explicitPath = std::getenv("SVMS_CONFIG")) {
        if (FileExists(explicitPath)) return explicitPath;
    }
    const std::string local = JoinPath(CurrentDirectory(), "config.json");
    if (FileExists(local)) return local;
    const std::string besideLibrary = JoinPath(libraryDirectory, "config.json");
    if (FileExists(besideLibrary)) return besideLibrary;
    std::string configHome;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) configHome = xdg;
    else if (const char* home = std::getenv("HOME"))
        configHome = JoinPath(home, ".config");
    const std::string roamingEquivalent = JoinPath(
        configHome, "SuperVirtualMIDISynth/config.json");
    return FileExists(roamingEquivalent) ? roamingEquivalent : std::string{};
}

bool LoadOptions(RuntimeOptions& options, std::string& error) {
    options.synth.sampleRate = 44100u;
    options.synth.maxVoices = 4096u;
    options.synth.renderThreads = 0u;
    options.synth.maxBlockFrames = options.bufferFrames;
    options.synth.masterVolume = 1.0f;
    options.synth.limiterEnabled = true;
    options.synth.limiterAlgorithm = svms::LimiterAlgorithm::Classic;
    options.synth.limiterThreshold = 0.95f;
    options.synth.limiterLookaheadMs = 3.0f;
    options.synth.limiterAttackMs = 0.5f;
    options.synth.limiterReleaseMs = 100.0f;

    const std::string libraryDirectory = LibraryDirectory();
    options.configPath = LocateConfig(libraryDirectory);
    std::string soundfont = "gm.sf2";
    if (!options.configPath.empty()) {
        try {
            std::ifstream stream(options.configPath);
            json root;
            stream >> root;
            if (root.is_object()) {
                if (auto audio = root.find("audio");
                    audio != root.end() && audio->is_object()) {
                    ReadNumber(*audio, "sample_rate", 8000u, 384000u,
                               options.synth.sampleRate);
                    ReadNumber(*audio, "buffer_frames", 16u, 8192u,
                               options.bufferFrames);
                    const auto linuxDevice = audio->find("linux_device");
                    if (linuxDevice != audio->end() && linuxDevice->is_string())
                        options.pcmDevice = linuxDevice->get<std::string>();
                }
                if (auto synth = root.find("synth");
                    synth != root.end() && synth->is_object()) {
                    ReadNumber(*synth, "max_voices", 1u, svms::kMaxPolyphony,
                               options.synth.maxVoices);
                    ReadNumber(*synth, "render_threads", 0u, 64u,
                               options.synth.renderThreads);
                    ReadNumber(*synth, "master_volume", 0.0f, 4.0f,
                               options.synth.masterVolume);
                    const auto selected = synth->find("soundfont");
                    if (selected != synth->end() && selected->is_string())
                        soundfont = selected->get<std::string>();
                }
                if (auto events = root.find("events");
                    events != root.end() && events->is_object()) {
                    ReadNumber(*events, "ring_capacity", 4096u,
                               svms::kMaxConfigurableEventCapacity,
                               options.eventCapacity);
                }
                if (auto limiter = root.find("limiter");
                    limiter != root.end() && limiter->is_object()) {
                    ReadBoolean(*limiter, "enabled",
                                options.synth.limiterEnabled);
                    ReadNumber(*limiter, "threshold", 0.1f, 1.0f,
                               options.synth.limiterThreshold);
                    ReadNumber(*limiter, "lookahead_ms", 0.0f, 20.0f,
                               options.synth.limiterLookaheadMs);
                    ReadNumber(*limiter, "attack_ms", 0.01f, 100.0f,
                               options.synth.limiterAttackMs);
                    ReadNumber(*limiter, "release_ms", 1.0f, 5000.0f,
                               options.synth.limiterReleaseMs);
                    const auto algorithm = limiter->find("algorithm");
                    if (algorithm != limiter->end() && algorithm->is_string() &&
                        algorithm->get<std::string>() == "adaptive")
                        options.synth.limiterAlgorithm =
                            svms::LimiterAlgorithm::Adaptive;
                }
            }
        } catch (const std::exception& exception) {
            error = std::string("cannot parse config.json: ") + exception.what();
            return false;
        }
    }

    if (const char* environmentSoundfont = std::getenv("SVMS_SOUNDFONT"))
        soundfont = environmentSoundfont;
    if (const char* environmentDevice = std::getenv("SVMS_AUDIO_DEVICE"))
        options.pcmDevice = environmentDevice;

    const std::string configDirectory = options.configPath.empty()
        ? CurrentDirectory() : DirectoryName(options.configPath);
    std::string resolvedSoundfont = IsAbsolute(soundfont)
        ? soundfont : JoinPath(configDirectory, soundfont);
    if (!FileExists(resolvedSoundfont)) {
        const std::string besideLibrary = JoinPath(libraryDirectory, soundfont);
        if (FileExists(besideLibrary)) resolvedSoundfont = besideLibrary;
    }
    if (!FileExists(resolvedSoundfont)) {
        error = "SoundFont not found: " + resolvedSoundfont +
                " (set synth.soundfont or SVMS_SOUNDFONT)";
        return false;
    }
    try {
        options.synth.soundfont = Utf8ToWide(resolvedSoundfont);
    } catch (const std::range_error&) {
        error = "SoundFont path is not valid UTF-8";
        return false;
    }
    options.synth.maxBlockFrames = options.bufferFrames;
    return true;
}

class PcmOutput {
public:
    ~PcmOutput() { Close(); }

    bool Open(const RuntimeOptions& options, std::string& error) {
        int result = snd_pcm_open(&pcm_, options.pcmDevice.c_str(),
                                  SND_PCM_STREAM_PLAYBACK, 0);
        if (result < 0) {
            error = std::string("cannot open ALSA PCM '") + options.pcmDevice +
                    "': " + snd_strerror(result);
            return false;
        }
        const uint32_t latencyUs = uint32_t(
            (uint64_t(options.bufferFrames) * 4u * 1000000u) /
            options.synth.sampleRate);
        result = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_FLOAT_LE,
                                    SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                                    options.synth.sampleRate, 1, latencyUs);
        if (result < 0) {
            error = std::string("cannot configure ALSA PCM: ") +
                    snd_strerror(result);
            Close();
            return false;
        }
        snd_pcm_uframes_t buffer = 0, period = 0;
        if (snd_pcm_get_params(pcm_, &buffer, &period) == 0) {
            bufferFrames_ = uint32_t((std::min<snd_pcm_uframes_t>)(
                buffer, UINT32_MAX));
            periodFrames_ = uint32_t((std::min<snd_pcm_uframes_t>)(
                period, UINT32_MAX));
        }
        return true;
    }

    void Close() {
        if (pcm_) {
            snd_pcm_drop(pcm_);
            snd_pcm_close(pcm_);
        }
        pcm_ = nullptr;
    }

    bool Write(const float* samples, uint32_t frames,
               const std::atomic<bool>& running) {
        uint32_t written = 0u;
        while (written < frames && running.load(std::memory_order_acquire)) {
            snd_pcm_sframes_t result = snd_pcm_writei(
                pcm_, samples + size_t(written) * 2u, frames - written);
            if (result == -EINTR) continue;
            if (result < 0) {
                result = snd_pcm_recover(pcm_, int(result), 1);
                if (result < 0) return false;
                continue;
            }
            written += uint32_t(result);
        }
        return written == frames;
    }

    uint64_t LeadFrames(uint32_t quantum) const {
        return uint64_t(bufferFrames_) + periodFrames_ + quantum;
    }

private:
    snd_pcm_t* pcm_ = nullptr;
    uint32_t bufferFrames_ = 0u, periodFrames_ = 0u;
};

enum class EventKind : uint32_t { Midi, Reset };

struct QueuedEvent {
    uint64_t timestampNs = 0u;
    uint64_t sequence = 0u;
    uint64_t targetFrame = 0u;
    uint32_t message = 0u;
    EventKind kind = EventKind::Midi;
    bool absoluteFrame = false;
};

uint64_t TimestampToFrame(uint64_t timestamp, uint64_t epoch,
                          uint32_t sampleRate, uint64_t leadFrames) {
    if (timestamp <= epoch) return leadFrames;
    const unsigned __int128 scaled =
        static_cast<unsigned __int128>(timestamp - epoch) * sampleRate;
    return leadFrames + uint64_t(scaled / 1000000000ull);
}

class KdmapiRuntime {
public:
    ~KdmapiRuntime() { Shutdown(); }

    bool Initialize() {
        std::lock_guard<std::mutex> guard(controlMutex_);
        if (running_.load(std::memory_order_acquire)) return true;
        if (audioThread_.joinable()) audioThread_.join();
        pcm_.reset();
        synth_.reset();
        RuntimeOptions options;
        std::string error;
        if (!LoadOptions(options, error)) {
            std::fprintf(stderr, "[SVMS Linux] %s\n", error.c_str());
            return false;
        }
        std::unique_ptr<svms::StandaloneSynth> synth(
            new (std::nothrow) svms::StandaloneSynth());
        if (!synth || !synth->Initialize(options.synth, error)) {
            std::fprintf(stderr, "[SVMS Linux] synth initialization failed: %s\n",
                         error.c_str());
            return false;
        }
        std::unique_ptr<PcmOutput> pcm(new (std::nothrow) PcmOutput());
        if (!pcm || !pcm->Open(options, error)) {
            std::fprintf(stderr, "[SVMS Linux] audio initialization failed: %s\n",
                         error.c_str());
            return false;
        }
        if (!queue_.ConfigureCapacity(options.eventCapacity)) {
            std::fprintf(stderr, "[SVMS Linux] cannot allocate MIDI queue\n");
            return false;
        }
        options_ = options;
        synth_ = std::move(synth);
        pcm_ = std::move(pcm);
        epochNs_ = MonotonicNanoseconds();
        leadFrames_ = pcm_->LeadFrames(options_.bufferFrames);
        ingressSequence_.store(0u, std::memory_order_relaxed);
        renderingTimeBits_.store(0u, std::memory_order_relaxed);
        activeVoices_.store(0u, std::memory_order_relaxed);
        freeVoices_.store(options_.synth.maxVoices, std::memory_order_relaxed);
        voiceSteals_.store(0u, std::memory_order_relaxed);
        callbackCount_.store(0u, std::memory_order_relaxed);
        submittedEvents_.store(0u, std::memory_order_relaxed);
        acceptedEvents_.store(0u, std::memory_order_relaxed);
        dispatchedEvents_.store(0u, std::memory_order_relaxed);
        noteCalls_.store(0u, std::memory_order_relaxed);
        matchedNotes_.store(0u, std::memory_order_relaxed);
        outputFramePublished_.store(0u, std::memory_order_relaxed);
        pendingCount_.store(0u, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        try {
            audioThread_ = std::thread(&KdmapiRuntime::AudioThread, this);
        } catch (...) {
            running_.store(false, std::memory_order_release);
            pcm_.reset();
            synth_.reset();
            return false;
        }
        std::fprintf(stderr,
            "[SVMS Linux] KDMAPI ready: SF2 '%ls', PCM '%s', %u Hz, "
            "%u frames, %u voices, %s, queue %u\n",
            options_.synth.soundfont.c_str(), options_.pcmDevice.c_str(),
            options_.synth.sampleRate, options_.bufferFrames,
            options_.synth.maxVoices, synth_->Backend(),
            options_.eventCapacity);
        return true;
    }

    bool Submit(uint32_t message, EventKind kind = EventKind::Midi) {
        return SubmitAt(message, MonotonicNanoseconds(), kind);
    }

    bool SubmitAt(uint32_t message, uint64_t timestampNs,
                  EventKind kind = EventKind::Midi) {
        return SubmitAtCancellable(message, timestampNs, kind, nullptr, 0u);
    }

    bool SubmitAtCancellable(
        uint32_t message, uint64_t timestampNs, EventKind kind,
        const std::atomic<uint64_t>* cancellation,
        uint64_t cancellationToken) {
        if (!running_.load(std::memory_order_acquire)) return false;
        submittedEvents_.fetch_add(1u, std::memory_order_relaxed);
        QueuedEvent event{};
        event.timestampNs = timestampNs ? timestampNs : MonotonicNanoseconds();
        event.sequence = ingressSequence_.fetch_add(
            1u, std::memory_order_relaxed);
        event.message = message;
        event.kind = kind;
        while (running_.load(std::memory_order_acquire)) {
            if (cancellation && cancellation->load(
                    std::memory_order_acquire) == cancellationToken)
                return false;
            if (queue_.TryPush(event)) {
                acceptedEvents_.fetch_add(1u, std::memory_order_relaxed);
                return true;
            }
            sched_yield();
        }
        return false;
    }

    bool SubmitAtFrame(uint32_t message, uint64_t outputFrame,
                       EventKind kind = EventKind::Midi) {
        return SubmitAtFrameCancellable(message, outputFrame, kind, nullptr,
                                        0u);
    }

    bool SubmitAtFrameCancellable(
        uint32_t message, uint64_t outputFrame, EventKind kind,
        const std::atomic<uint64_t>* cancellation,
        uint64_t cancellationToken) {
        if (!running_.load(std::memory_order_acquire)) return false;
        submittedEvents_.fetch_add(1u, std::memory_order_relaxed);
        QueuedEvent event{};
        event.sequence = ingressSequence_.fetch_add(
            1u, std::memory_order_relaxed);
        event.targetFrame = outputFrame;
        event.message = message;
        event.kind = kind;
        event.absoluteFrame = true;
        while (running_.load(std::memory_order_acquire)) {
            if (cancellation && cancellation->load(
                    std::memory_order_acquire) == cancellationToken)
                return false;
            if (queue_.TryPush(event)) {
                acceptedEvents_.fetch_add(1u, std::memory_order_relaxed);
                return true;
            }
            sched_yield();
        }
        return false;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> guard(controlMutex_);
        running_.store(false, std::memory_order_release);
        if (audioThread_.joinable()) audioThread_.join();
        pcm_.reset();
        synth_.reset();
        activeVoices_.store(0u, std::memory_order_relaxed);
        freeVoices_.store(0u, std::memory_order_relaxed);
    }

    svms::SnappyVoiceStatistics Statistics() const {
        svms::SnappyVoiceStatistics result{};
        result.activeVoices = activeVoices_.load(std::memory_order_relaxed);
        result.freeVoices = freeVoices_.load(std::memory_order_relaxed);
        result.voiceSteals = voiceSteals_.load(std::memory_order_relaxed);
        return result;
    }

    float RenderingTime() const {
        const uint32_t bits = renderingTimeBits_.load(std::memory_order_relaxed);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool Running() const { return running_.load(std::memory_order_acquire); }
    uint32_t SampleRate() const { return options_.synth.sampleRate; }
    uint32_t BufferFrames() const { return options_.bufferFrames; }
    uint64_t CallbackCount() const {
        return callbackCount_.load(std::memory_order_relaxed);
    }
    uint64_t SubmittedEvents() const {
        return submittedEvents_.load(std::memory_order_relaxed);
    }
    uint64_t AcceptedEvents() const {
        return acceptedEvents_.load(std::memory_order_relaxed);
    }
    uint64_t DispatchedEvents() const {
        return dispatchedEvents_.load(std::memory_order_relaxed);
    }
    uint64_t NoteCalls() const {
        return noteCalls_.load(std::memory_order_relaxed);
    }
    uint64_t MatchedNotes() const {
        return matchedNotes_.load(std::memory_order_relaxed);
    }
    uint64_t NextOutputFrame() const {
        return outputFramePublished_.load(std::memory_order_acquire);
    }
    uint32_t QueueSize() const { return queue_.Size(); }
    uint32_t QueueCapacity() const { return queue_.CapacityValue(); }
    uint32_t PendingCount() const {
        return pendingCount_.load(std::memory_order_relaxed);
    }

private:
    void PublishStatistics(float milliseconds) {
        activeVoices_.store(synth_->Active(), std::memory_order_relaxed);
        freeVoices_.store(synth_->Free(), std::memory_order_relaxed);
        voiceSteals_.store(synth_->Steals(), std::memory_order_relaxed);
        noteCalls_.store(synth_->NoteCalls(), std::memory_order_relaxed);
        matchedNotes_.store(synth_->MatchedNotes(), std::memory_order_relaxed);
        uint32_t bits = 0u;
        std::memcpy(&bits, &milliseconds, sizeof(bits));
        renderingTimeBits_.store(bits, std::memory_order_relaxed);
    }

    void AudioThread() {
        std::vector<QueuedEvent> pending;
        pending.reserve(options_.eventCapacity);
        std::vector<float> left(options_.bufferFrames);
        std::vector<float> right(options_.bufferFrames);
        std::vector<float> interleaved(size_t(options_.bufferFrames) * 2u);
        size_t pendingHead = 0u;
        uint64_t outputFrame = 0u;
        while (running_.load(std::memory_order_acquire)) {
            const auto renderStart = std::chrono::steady_clock::now();
            if (pendingHead && (pendingHead == pending.size() ||
                                pendingHead >= pending.size() / 2u)) {
                pending.erase(pending.begin(), pending.begin() + pendingHead);
                pendingHead = 0u;
            }
            QueuedEvent event{};
            while (pending.size() - pendingHead < options_.eventCapacity &&
                   queue_.TryPop(event)) {
                if (!event.absoluteFrame) {
                    event.targetFrame = TimestampToFrame(
                        event.timestampNs, epochNs_, options_.synth.sampleRate,
                        leadFrames_);
                }
                pending.push_back(event);
            }
            pendingCount_.store(static_cast<uint32_t>(
                pending.size() - pendingHead), std::memory_order_relaxed);
            if (pending.size() - pendingHead > 1u) {
                std::sort(pending.begin() + pendingHead, pending.end(),
                    [](const QueuedEvent& leftEvent,
                       const QueuedEvent& rightEvent) {
                        if (leftEvent.targetFrame != rightEvent.targetFrame)
                            return leftEvent.targetFrame < rightEvent.targetFrame;
                        return leftEvent.sequence < rightEvent.sequence;
                    });
            }
            const uint64_t blockEnd = outputFrame + options_.bufferFrames;
            uint32_t cursor = 0u;
            while (pendingHead < pending.size() &&
                   pending[pendingHead].targetFrame < blockEnd) {
                const QueuedEvent& due = pending[pendingHead++];
                const uint64_t writableFrame = outputFrame + cursor;
                const uint64_t target = (std::max)(due.targetFrame,
                                                   writableFrame);
                const uint32_t span = uint32_t(target - writableFrame);
                if (span) {
                    synth_->Render(left.data() + cursor, right.data() + cursor,
                                   span, writableFrame);
                    cursor += span;
                }
                if (due.kind == EventKind::Reset)
                    synth_->ResetAll(target);
                else
                    synth_->Dispatch(due.message, target);
                dispatchedEvents_.fetch_add(1u, std::memory_order_relaxed);
            }
            if (cursor < options_.bufferFrames) {
                synth_->Render(left.data() + cursor, right.data() + cursor,
                               options_.bufferFrames - cursor,
                               outputFrame + cursor);
            }
            for (uint32_t frame = 0u; frame < options_.bufferFrames; ++frame) {
                interleaved[size_t(frame) * 2u] = left[frame];
                interleaved[size_t(frame) * 2u + 1u] = right[frame];
            }
            const auto renderEnd = std::chrono::steady_clock::now();
            const float milliseconds = float(
                std::chrono::duration<double, std::milli>(
                    renderEnd - renderStart).count());
            PublishStatistics(milliseconds);
            callbackCount_.fetch_add(1u, std::memory_order_relaxed);
            if (!pcm_->Write(interleaved.data(), options_.bufferFrames,
                             running_)) {
                std::fprintf(stderr, "[SVMS Linux] ALSA output stopped\n");
                running_.store(false, std::memory_order_release);
                break;
            }
            outputFrame = blockEnd;
            outputFramePublished_.store(outputFrame,
                                        std::memory_order_release);
        }
    }

    mutable std::mutex controlMutex_;
    svms::DynamicMPSCQueue<QueuedEvent> queue_;
    RuntimeOptions options_{};
    std::unique_ptr<svms::StandaloneSynth> synth_;
    std::unique_ptr<PcmOutput> pcm_;
    std::thread audioThread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> ingressSequence_{0u};
    std::atomic<uint32_t> renderingTimeBits_{0u};
    std::atomic<uint32_t> activeVoices_{0u};
    std::atomic<uint32_t> freeVoices_{0u};
    std::atomic<uint32_t> voiceSteals_{0u};
    std::atomic<uint64_t> callbackCount_{0u};
    std::atomic<uint64_t> submittedEvents_{0u};
    std::atomic<uint64_t> acceptedEvents_{0u};
    std::atomic<uint64_t> dispatchedEvents_{0u};
    std::atomic<uint64_t> noteCalls_{0u};
    std::atomic<uint64_t> matchedNotes_{0u};
    std::atomic<uint64_t> outputFramePublished_{0u};
    std::atomic<uint32_t> pendingCount_{0u};
    uint64_t epochNs_ = 0u;
    uint64_t leadFrames_ = 0u;
};

KdmapiRuntime& Runtime() {
    static KdmapiRuntime runtime;
    return runtime;
}

constexpr uint32_t kNativeSessionCapacity = 64u;
std::atomic<uint64_t> gNativeSessions[kNativeSessionCapacity]{};
std::atomic<uint64_t> gNativeSessionCancellation[kNativeSessionCapacity]{};
std::atomic<uint32_t> gNativeSessionGeneration{1u};
svms::NativeOfflineSessions gNativeOfflineSessions;
std::atomic<bool> gKdmapiOwner{false};
std::mutex gFrontendMutex;

bool NativeSessionIsValid(SVMS_Session session) {
    const uint32_t encodedIndex = static_cast<uint32_t>(session);
    return encodedIndex != 0u && encodedIndex <= kNativeSessionCapacity &&
        gNativeSessions[encodedIndex - 1u].load(std::memory_order_acquire) ==
            session;
}

std::atomic<uint64_t>* NativeSessionCancellation(SVMS_Session session) {
    const uint32_t encodedIndex = static_cast<uint32_t>(session);
    if (!encodedIndex || encodedIndex > kNativeSessionCapacity ||
        gNativeSessions[encodedIndex - 1u].load(std::memory_order_acquire) !=
            session)
        return nullptr;
    return &gNativeSessionCancellation[encodedIndex - 1u];
}

SVMS_Result NativeSubmissionResult(bool accepted,
                                   const std::atomic<uint64_t>* cancellation,
                                   SVMS_Session session) {
    if (accepted) return SVMS_RESULT_OK;
    return cancellation && cancellation->load(std::memory_order_acquire) ==
            session
        ? SVMS_RESULT_CANCELLED : SVMS_RESULT_INTERNAL_ERROR;
}

bool AnyNativeSessions() {
    for (const auto& slot : gNativeSessions)
        if (slot.load(std::memory_order_acquire) != 0u) return true;
    return false;
}

void MaybeShutdownRuntime() {
    if (!gKdmapiOwner.load(std::memory_order_acquire) && !AnyNativeSessions())
        Runtime().Shutdown();
}

SVMS_Result NativeCreateSession(const SVMS_SessionConfig* config,
                                SVMS_Session* outSession) {
    if (!outSession) return SVMS_RESULT_INVALID_ARGUMENT;
    *outSession = 0u;
    if (config && (config->struct_size < 16u ||
                   config->struct_version != SVMS_STRUCT_VERSION_1 ||
                   config->flags != 0u))
        return SVMS_RESULT_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(gFrontendMutex);
    if (!Runtime().Initialize()) return SVMS_RESULT_INTERNAL_ERROR;
    uint32_t generation = gNativeSessionGeneration.fetch_add(
        1u, std::memory_order_relaxed) + 1u;
    if (!generation) generation = gNativeSessionGeneration.fetch_add(
        1u, std::memory_order_relaxed) + 1u;
    for (uint32_t i = 0u; i < kNativeSessionCapacity; ++i) {
        const uint64_t token = (uint64_t(generation) << 32u) | uint64_t(i + 1u);
        uint64_t empty = 0u;
        if (gNativeSessions[i].compare_exchange_strong(
                empty, token, std::memory_order_release,
                std::memory_order_relaxed)) {
            *outSession = token;
            return SVMS_RESULT_OK;
        }
    }
    MaybeShutdownRuntime();
    return SVMS_RESULT_NO_RESOURCES;
}

SVMS_Result NativeDestroySession(SVMS_Session session) {
    if (gNativeOfflineSessions.IsToken(session))
        return gNativeOfflineSessions.Destroy(session);
    const uint32_t encodedIndex = static_cast<uint32_t>(session);
    if (!encodedIndex || encodedIndex > kNativeSessionCapacity)
        return SVMS_RESULT_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> guard(gFrontendMutex);
    if (gNativeSessions[encodedIndex - 1u].load(std::memory_order_acquire) !=
        session)
        return SVMS_RESULT_INVALID_ARGUMENT;
    gNativeSessionCancellation[encodedIndex - 1u].store(
        session, std::memory_order_release);
    uint64_t expected = session;
    if (!gNativeSessions[encodedIndex - 1u].compare_exchange_strong(
            expected, 0u, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return SVMS_RESULT_INVALID_ARGUMENT;
    MaybeShutdownRuntime();
    return SVMS_RESULT_OK;
}

SVMS_Result NativeSendShort(SVMS_Session session, uint32_t message) {
    std::atomic<uint64_t>* cancellation = NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    return NativeSubmissionResult(Runtime().SubmitAtCancellable(
        message, MonotonicNanoseconds(), EventKind::Midi, cancellation,
        session), cancellation, session);
}

SVMS_Result NativeSendShortAtClock(SVMS_Session session, uint32_t message,
                                   uint64_t timestampNs) {
    std::atomic<uint64_t>* cancellation = NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    return NativeSubmissionResult(Runtime().SubmitAtCancellable(
        message, timestampNs, EventKind::Midi, cancellation, session),
        cancellation, session);
}

SVMS_Result NativeSendShortBatch(SVMS_Session session,
                                 const SVMS_ShortEvent* events,
                                 uint32_t eventCount) {
    std::atomic<uint64_t>* cancellation = NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    if (!events && eventCount) return SVMS_RESULT_INVALID_ARGUMENT;
    for (uint32_t i = 0u; i < eventCount; ++i)
        if (events[i].reserved) return SVMS_RESULT_INVALID_ARGUMENT;
    for (uint32_t i = 0u; i < eventCount; ++i)
        if (!Runtime().SubmitAtCancellable(
                events[i].packed_message, events[i].timestamp_qpc,
                EventKind::Midi, cancellation, session))
            return NativeSubmissionResult(false, cancellation, session);
    return SVMS_RESULT_OK;
}

SVMS_Result NativeSendSystemExclusive(SVMS_Session session, const uint8_t*,
                                      uint32_t) {
    std::atomic<uint64_t>* cancellation = NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    return SVMS_RESULT_UNSUPPORTED;
}

SVMS_Result NativeReset(SVMS_Session session) {
    if (gNativeOfflineSessions.IsToken(session))
        return gNativeOfflineSessions.Reset(session);
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    return Runtime().Submit(0u, EventKind::Reset) ? SVMS_RESULT_OK
                                                  : SVMS_RESULT_INTERNAL_ERROR;
}

SVMS_Result NativeGetTelemetry(SVMS_Session session,
                               SVMS_TelemetryV1* telemetry) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!telemetry || telemetry->struct_size < sizeof(SVMS_TelemetryV1) ||
        telemetry->struct_version != SVMS_STRUCT_VERSION_1)
        return SVMS_RESULT_INVALID_ARGUMENT;
    const svms::SnappyVoiceStatistics voices = Runtime().Statistics();
    SVMS_TelemetryV1 result{};
    result.struct_size = sizeof(result);
    result.struct_version = SVMS_STRUCT_VERSION_1;
    result.callback_count = Runtime().CallbackCount();
    result.submitted_events = Runtime().SubmittedEvents();
    result.accepted_events = Runtime().AcceptedEvents();
    result.dispatched_events = Runtime().DispatchedEvents();
    result.note_ons = Runtime().NoteCalls();
    result.matched_regions = Runtime().MatchedNotes();
    result.configured_voices = Runtime().MatchedNotes();
    result.voice_steals = voices.voiceSteals;
    result.active_voices = voices.activeVoices;
    result.free_voices = voices.freeVoices;
    result.sample_rate = Runtime().SampleRate();
    result.buffer_frames = Runtime().BufferFrames();
    result.soundfont_loaded = Runtime().Running() ? 1u : 0u;
    result.audio_running = Runtime().Running() ? 1u : 0u;
    result.render_time_ms = Runtime().RenderingTime();
    *telemetry = result;
    return SVMS_RESULT_OK;
}

SVMS_Result NativeGetRuntimeClock(uint64_t* ticks, uint64_t* frequency) {
    if (!ticks || !frequency) return SVMS_RESULT_INVALID_ARGUMENT;
    *ticks = MonotonicNanoseconds();
    *frequency = 1000000000ull;
    return SVMS_RESULT_OK;
}

SVMS_Result NativeGetMonotonicClock(uint64_t* nanoseconds) {
    if (!nanoseconds) return SVMS_RESULT_INVALID_ARGUMENT;
    *nanoseconds = MonotonicNanoseconds();
    return SVMS_RESULT_OK;
}

SVMS_Result NativeGetOutputClock(SVMS_Session session,
                                 uint64_t* nextOutputFrame,
                                 uint32_t* sampleRate) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!nextOutputFrame || !sampleRate) return SVMS_RESULT_INVALID_ARGUMENT;
    *nextOutputFrame = Runtime().NextOutputFrame();
    *sampleRate = Runtime().SampleRate();
    return SVMS_RESULT_OK;
}

SVMS_Result NativeSendTimedShortBatch(SVMS_Session session,
                                      const SVMS_TimedShortEvent* events,
                                      uint32_t eventCount) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!events && eventCount) return SVMS_RESULT_INVALID_ARGUMENT;
    for (uint32_t i = 0u; i < eventCount; ++i) {
        if (events[i].reserved ||
            events[i].timestamp_domain > SVMS_TIMESTAMP_MONOTONIC_NS ||
            events[i].timestamp_domain == SVMS_TIMESTAMP_QPC)
            return SVMS_RESULT_INVALID_ARGUMENT;
    }
    const uint64_t immediate = MonotonicNanoseconds();
    for (uint32_t i = 0u; i < eventCount; ++i) {
        const SVMS_TimedShortEvent& event = events[i];
        const bool accepted = event.timestamp_domain ==
                SVMS_TIMESTAMP_OUTPUT_FRAME
            ? Runtime().SubmitAtFrameCancellable(
                event.packed_message, event.timestamp, EventKind::Midi,
                cancellation, session)
            : Runtime().SubmitAtCancellable(event.packed_message,
                event.timestamp_domain == SVMS_TIMESTAMP_IMMEDIATE
                    ? immediate : event.timestamp, EventKind::Midi,
                cancellation, session);
        if (!accepted)
            return NativeSubmissionResult(false, cancellation, session);
    }
    return SVMS_RESULT_OK;
}

SVMS_Result NativeGetQueueInfo(SVMS_Session session,
                               SVMS_QueueInfo* queueInfo) {
    if (!NativeSessionIsValid(session)) return SVMS_RESULT_NOT_INITIALIZED;
    if (!queueInfo || queueInfo->struct_size < 16u ||
        queueInfo->struct_version != SVMS_STRUCT_VERSION_1)
        return SVMS_RESULT_INVALID_ARGUMENT;
    const uint32_t callerSize = queueInfo->struct_size;
    SVMS_QueueInfo result{};
    result.struct_size = sizeof(result);
    result.struct_version = SVMS_STRUCT_VERSION_1;
    result.ingress_mode = SVMS_INGRESS_LOSSLESS;
    result.current_velocity_cutoff = 1u;
    result.queue_capacity = Runtime().QueueCapacity();
    result.raw_ingress_count = Runtime().QueueSize();
    result.scheduled_count = Runtime().PendingCount();
    result.max_events_per_callback = UINT64_MAX;
    result.submitted_events = Runtime().SubmittedEvents();
    result.accepted_events = Runtime().AcceptedEvents();
    std::memcpy(queueInfo, &result,
                (std::min)(callerSize,
                           static_cast<uint32_t>(sizeof(result))));
    return SVMS_RESULT_OK;
}

SVMS_Result NativeCreateOfflineSession(
    const SVMS_OfflineSessionConfig* config, const char* soundfontPathUtf8,
    SVMS_Session* outSession) {
    if (!soundfontPathUtf8 || !*soundfontPathUtf8)
        return SVMS_RESULT_INVALID_ARGUMENT;
    try {
        return gNativeOfflineSessions.Create(
            config, Utf8ToWide(soundfontPathUtf8), outSession);
    } catch (...) {
        return SVMS_RESULT_INVALID_ARGUMENT;
    }
}

SVMS_Result NativeRenderOffline(
    SVMS_Session session, const SVMS_OfflineEvent* events,
    uint32_t eventCount, float* outputLeft, float* outputRight,
    uint32_t frameCount) {
    return gNativeOfflineSessions.Render(session, events, eventCount,
                                          outputLeft, outputRight,
                                          frameCount);
}

SVMS_Result NativeGetOfflineTelemetry(
    SVMS_Session session, SVMS_OfflineTelemetry* telemetry) {
    return gNativeOfflineSessions.GetTelemetry(session, telemetry);
}

SVMS_Result NativeCancelSessionSubmissions(SVMS_Session session) {
    std::atomic<uint64_t>* cancellation = NativeSessionCancellation(session);
    if (!cancellation) return SVMS_RESULT_NOT_INITIALIZED;
    cancellation->store(session, std::memory_order_release);
    return SVMS_RESULT_OK;
}

} // namespace

#define SVMS_LINUX_EXPORT extern "C" __attribute__((visibility("default")))

SVMS_LINUX_EXPORT int IsKDMAPIAvailable() { return 1; }

SVMS_LINUX_EXPORT void* InitializeKDMAPIStream() {
    std::lock_guard<std::mutex> guard(gFrontendMutex);
    if (gKdmapiOwner.load(std::memory_order_acquire))
        return reinterpret_cast<void*>(1);
    if (!Runtime().Initialize()) return nullptr;
    gKdmapiOwner.store(true, std::memory_order_release);
    return reinterpret_cast<void*>(1);
}

SVMS_LINUX_EXPORT int TerminateKDMAPIStream() {
    std::lock_guard<std::mutex> guard(gFrontendMutex);
    gKdmapiOwner.store(false, std::memory_order_release);
    MaybeShutdownRuntime();
    return 1;
}

SVMS_LINUX_EXPORT void ResetKDMAPIStream() {
    Runtime().Submit(0u, EventKind::Reset);
}

SVMS_LINUX_EXPORT unsigned int ReturnKDMAPIVer(
        uint32_t* major, uint32_t* minor, uint32_t* build,
        uint32_t* revision) {
    if (major) *major = 4u;
    if (minor) *minor = 1u;
    if (build) *build = 0u;
    if (revision) *revision = 0u;
    return 1u;
}

SVMS_LINUX_EXPORT void SendDirectData(uint32_t message) {
    Runtime().Submit(message);
}

SVMS_LINUX_EXPORT void SendDirectDataNoBuf(uint32_t message) {
    Runtime().Submit(message);
}

SVMS_LINUX_EXPORT int SendDirectLongData(const void*, uint32_t) { return 0; }
SVMS_LINUX_EXPORT int SendDirectLongDataNoBuf(const void*, uint32_t) { return 0; }

SVMS_LINUX_EXPORT float GetRenderingTime() {
    return Runtime().RenderingTime();
}

SVMS_LINUX_EXPORT uint32_t GetVoiceCount() {
    return Runtime().Statistics().activeVoices;
}

// Ziggy's Linux loader expects this 12-byte structure by value. SysV AMD64
// returns active/free in RAX and steals in RDX, exactly as its caller reads it.
SVMS_LINUX_EXPORT svms::SnappyVoiceStatistics GetVoiceStatistics() {
    return Runtime().Statistics();
}

SVMS_LINUX_EXPORT SVMS_Result SVMS_CALL SVMS_GetInterface(
        uint32_t requestedAbi, uint32_t callerTableSize,
        SVMS_Interface* outInterface) {
    const uint32_t minimumSize = static_cast<uint32_t>(
        offsetof(SVMS_Interface, get_runtime_clock) +
        sizeof(SVMS_GetRuntimeClockFn));
    if (!outInterface || callerTableSize < minimumSize)
        return SVMS_RESULT_INVALID_ARGUMENT;
    if (requestedAbi != SVMS_ABI_VERSION_1)
        return SVMS_RESULT_UNSUPPORTED_ABI;
    SVMS_Interface table{};
    table.struct_size = sizeof(table);
    table.struct_version = SVMS_STRUCT_VERSION_1;
    table.abi_version = SVMS_ABI_VERSION_1;
    table.capabilities = SVMS_CAP_EXACT_MONOTONIC_NS |
        SVMS_CAP_SHORT_EVENT_BATCH | SVMS_CAP_TELEMETRY_V1 |
        SVMS_CAP_KDMAPI_FACADE | SVMS_CAP_EXACT_OUTPUT_FRAMES |
        SVMS_CAP_MIXED_TIMESTAMP_BATCH |
        SVMS_CAP_ISOLATED_OFFLINE_SESSIONS |
        SVMS_CAP_CANCELLABLE_SUBMISSION;
    table.product_major = svms::build::kProductMajor;
    table.product_minor = svms::build::kProductMinor;
    table.product_patch = svms::build::kProductPatch;
    table.build_number = svms::build::kBuildNumber;
    table.create_session = NativeCreateSession;
    table.destroy_session = NativeDestroySession;
    table.send_short = NativeSendShort;
    table.send_short_at_qpc = NativeSendShortAtClock;
    table.send_short_batch = NativeSendShortBatch;
    table.send_system_exclusive = NativeSendSystemExclusive;
    table.reset = NativeReset;
    table.get_telemetry = NativeGetTelemetry;
    table.get_runtime_clock = NativeGetRuntimeClock;
    table.send_timed_short_batch = NativeSendTimedShortBatch;
    table.get_output_clock = NativeGetOutputClock;
    table.get_monotonic_clock = NativeGetMonotonicClock;
    table.get_queue_info = NativeGetQueueInfo;
    table.panic = NativeReset;
    table.create_offline_session = NativeCreateOfflineSession;
    table.render_offline = NativeRenderOffline;
    table.get_offline_telemetry = NativeGetOfflineTelemetry;
    table.cancel_session_submissions = NativeCancelSessionSubmissions;
    std::memcpy(outInterface, &table,
                (std::min)(callerTableSize,
                           static_cast<uint32_t>(sizeof(table))));
    return SVMS_RESULT_OK;
}
