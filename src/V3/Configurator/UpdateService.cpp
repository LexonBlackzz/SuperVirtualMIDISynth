#include "UpdateService.h"
#include "SVMSBuildInfo.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace svms::cfg {
namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] =
    L"/repos/LexonBlackzz/SuperVirtualMIDISynth/releases?per_page=20";
constexpr uint64_t kCacheLifetimeSeconds = 6u * 60u * 60u;
constexpr size_t kMaximumResponseBytes = 2u * 1024u * 1024u;

uint64_t UnixTimeNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::seconds>(std::chrono::system_clock::now()
            .time_since_epoch()).count());
}

fs::path CachePath() {
    wchar_t localAppData[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                SHGFP_TYPE_CURRENT, localAppData)))
        return {};
    return fs::path(localAppData) / L"SuperVirtualMIDISynth" /
           L"release-check-cache.json";
}

bool ParseVersion(const std::string& tag, uint32_t& major, uint32_t& minor,
                  uint32_t& patch) {
    const char* p = tag.c_str();
    if (*p == 'v' || *p == 'V') ++p;
    unsigned int values[3]{};
    char trailing = 0;
    const int count = sscanf_s(p, "%u.%u.%u%c", &values[0], &values[1],
                               &values[2], &trailing, 1u);
    if (count < 2) return false;
    major = values[0];
    minor = values[1];
    patch = count >= 3 ? values[2] : 0u;
    return true;
}

int CompareVersion(uint32_t major, uint32_t minor, uint32_t patch) {
    const uint32_t remote[3] = {major, minor, patch};
    const uint32_t local[3] = {build::kProductMajor, build::kProductMinor,
                               build::kProductPatch};
    for (uint32_t i = 0u; i < 3u; ++i) {
        if (remote[i] < local[i]) return -1;
        if (remote[i] > local[i]) return 1;
    }
    return 0;
}

int CompareTriples(uint32_t aMajor, uint32_t aMinor, uint32_t aPatch,
                   uint32_t bMajor, uint32_t bMinor, uint32_t bPatch) {
    const uint32_t a[3] = {aMajor, aMinor, aPatch};
    const uint32_t b[3] = {bMajor, bMinor, bPatch};
    for (uint32_t i = 0u; i < 3u; ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

bool SelectRelease(const std::string& response, bool fromCache,
                   UpdateSnapshot& result) {
    const json releases = json::parse(response);
    if (!releases.is_array()) return false;
    const bool allowPrerelease = build::kReleaseChannelId == 2u;
    bool found = false;
    for (const json& release : releases) {
        if (!release.is_object() || release.value("draft", false)) continue;
        const bool prerelease = release.value("prerelease", false);
        if (prerelease && !allowPrerelease) continue;
        uint32_t major = 0u, minor = 0u, patch = 0u;
        const std::string tag = release.value("tag_name", std::string{});
        if (!ParseVersion(tag, major, minor, patch)) continue;
        if (found && CompareTriples(major, minor, patch, result.major,
                                    result.minor, result.patch) <= 0)
            continue;
        result.major = major;
        result.minor = minor;
        result.patch = patch;
        result.prerelease = prerelease;
        result.title = release.value("name", tag);
        result.releaseUrl = release.value("html_url", std::string{});
        found = true;
    }
    if (!found) return false;
    result.fromCache = fromCache;
    if (CompareVersion(result.major, result.minor, result.patch) > 0) {
        result.status = UpdateStatus::Available;
        result.message = "A newer release is available.";
    } else {
        result.status = UpdateStatus::UpToDate;
        result.message = "This configurator is up to date.";
    }
    return true;
}

bool LoadCache(std::string& response) {
    const fs::path path = CachePath();
    if (path.empty()) return false;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        json cached;
        input >> cached;
        const uint64_t checked = cached.value("checked_unix", 0ull);
        const uint64_t now = UnixTimeNow();
        if (checked == 0u || now < checked ||
            now - checked > kCacheLifetimeSeconds)
            return false;
        response = cached.value("response", std::string{});
        return !response.empty();
    } catch (...) {
        return false;
    }
}

void SaveCache(const std::string& response) {
    const fs::path path = CachePath();
    if (path.empty()) return;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.wstring() + L".tmp." +
                          std::to_wstring(GetCurrentProcessId());
    try {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) return;
        output << json{{"checked_unix", UnixTimeNow()},
                       {"response", response}}.dump();
        output.close();
        if (!output) return;
        if (!MoveFileExW(temp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            DeleteFileW(temp.c_str());
    } catch (...) {
        DeleteFileW(temp.c_str());
    }
}

bool DownloadReleaseList(std::string& output, std::string& error) {
    HINTERNET session = WinHttpOpen(
        L"SuperVirtualMIDISynth-Configurator/0.7",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "Could not initialize the update connection.";
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET connect = WinHttpConnect(session, kApiHost,
                                       INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(
        connect, L"GET", kApiPath, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = request && WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
        0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0u, statusSize = sizeof(status);
    if (ok) ok = WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
        WINHTTP_NO_HEADER_INDEX) && status == 200u;
    while (ok) {
        DWORD available = 0u;
        if (!WinHttpQueryDataAvailable(request, &available)) { ok = false; break; }
        if (available == 0u) break;
        if (output.size() + available > kMaximumResponseBytes) {
            error = "The release response was unexpectedly large.";
            ok = false;
            break;
        }
        const size_t oldSize = output.size();
        output.resize(oldSize + available);
        DWORD read = 0u;
        if (!WinHttpReadData(request, output.data() + oldSize, available,
                             &read)) {
            ok = false;
            break;
        }
        output.resize(oldSize + read);
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!ok && error.empty())
        error = status ? "GitHub returned HTTP " + std::to_string(status) + "."
                       : "The GitHub release check failed.";
    return ok;
}

} // namespace

UpdateService::~UpdateService() {
    if (worker_.joinable()) worker_.join();
}

void UpdateService::CheckAsync(bool ignoreCache) {
    if (checking_.exchange(true)) return;
    if (worker_.joinable()) worker_.join();
    Publish(UpdateSnapshot{UpdateStatus::Checking});
    worker_ = std::thread([this, ignoreCache] { CheckWorker(ignoreCache); });
}

UpdateSnapshot UpdateService::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void UpdateService::Publish(const UpdateSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = snapshot;
}

void UpdateService::CheckWorker(bool ignoreCache) {
    UpdateSnapshot result{};
    std::string response;
    try {
        if (!ignoreCache && LoadCache(response) &&
            SelectRelease(response, true, result)) {
            Publish(result);
            checking_.store(false);
            return;
        }
        std::string error;
        if (!DownloadReleaseList(response, error)) {
            result.status = UpdateStatus::Failed;
            result.message = error;
        } else if (!SelectRelease(response, false, result)) {
            result.status = UpdateStatus::Failed;
            result.message = "No compatible release was found.";
        } else {
            SaveCache(response);
        }
    } catch (const std::exception& e) {
        result.status = UpdateStatus::Failed;
        result.message = std::string("Release data was invalid: ") + e.what();
    }
    Publish(result);
    checking_.store(false);
}

} // namespace svms::cfg
