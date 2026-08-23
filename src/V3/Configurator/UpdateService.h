#ifndef SVMS_CONFIGURATOR_UPDATESERVICE_H
#define SVMS_CONFIGURATOR_UPDATESERVICE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace svms::cfg {

enum class UpdateStatus {
    Idle,
    Checking,
    UpToDate,
    Available,
    Failed,
};

struct UpdateSnapshot {
    UpdateStatus status = UpdateStatus::Idle;
    uint32_t major = 0u;
    uint32_t minor = 0u;
    uint32_t patch = 0u;
    bool prerelease = false;
    bool fromCache = false;
    std::string title;
    std::string releaseUrl;
    std::string message;
};

// Configurator-only network service. It never starts on its own and never runs
// in the driver. A user request starts one bounded background HTTP operation.
class UpdateService {
public:
    UpdateService() = default;
    ~UpdateService();
    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    void CheckAsync(bool ignoreCache = false);
    UpdateSnapshot GetSnapshot() const;

private:
    void CheckWorker(bool ignoreCache);
    void Publish(const UpdateSnapshot& snapshot);

    mutable std::mutex mutex_;
    UpdateSnapshot snapshot_{};
    std::thread worker_;
    std::atomic<bool> checking_{false};
};

} // namespace svms::cfg

#endif
