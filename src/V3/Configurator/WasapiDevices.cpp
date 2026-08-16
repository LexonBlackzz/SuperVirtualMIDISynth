#include "WasapiDevices.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <objbase.h>

#include <algorithm>
#include <cctype>

namespace svms::cfg {

namespace {

std::wstring Widen(const char* s) {
    if (!s || !*s) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, result.data(), len);
    return result;
}

bool ContainsCI(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (towlower(haystack[i + j]) != towlower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

} // namespace

WasapiDeviceList::WasapiDeviceList() = default;
WasapiDeviceList::~WasapiDeviceList() = default;

void WasapiDeviceList::Enumerate() {
    devices_.clear();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comOwned = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        if (comOwned) CoUninitialize();
        return;
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        if (comOwned) CoUninitialize();
        return;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        collection->Item(i, &device);
        if (!device) continue;

        LPWSTR deviceId = nullptr;
        device->GetId(&deviceId);

        IPropertyStore* props = nullptr;
        device->OpenPropertyStore(STGM_READ, &props);

        std::wstring friendlyName;
        if (props) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            props->GetValue(PKEY_Device_FriendlyName, &varName);
            if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                friendlyName = varName.pwszVal;
            }
            PropVariantClear(&varName);
            props->Release();
        }

        AudioDevice dev;
        dev.id = deviceId ? deviceId : L"";
        dev.friendlyName = friendlyName.empty() ? L"Unknown Device" : friendlyName;
        dev.isDefault = false;
        devices_.push_back(std::move(dev));

        if (deviceId) CoTaskMemFree(deviceId);
        device->Release();
    }

    collection->Release();

    IMMDevice* defaultDevice = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
    if (SUCCEEDED(hr) && defaultDevice) {
        LPWSTR defaultId = nullptr;
        defaultDevice->GetId(&defaultId);
        for (auto& dev : devices_) {
            if (defaultId && dev.id == defaultId) {
                dev.isDefault = true;
                break;
            }
        }
        if (defaultId) CoTaskMemFree(defaultId);
        defaultDevice->Release();
    }

    enumerator->Release();
    if (comOwned) CoUninitialize();
}

std::vector<std::string> WasapiDeviceList::FriendlyNames() const {
    std::vector<std::string> names;
    names.reserve(devices_.size() + 1);
    names.push_back("Default Windows Output Device");
    for (const auto& dev : devices_) {
        int len = WideCharToMultiByte(CP_UTF8, 0, dev.friendlyName.data(),
                                      static_cast<int>(dev.friendlyName.size()),
                                      nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string s(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, dev.friendlyName.data(),
                                static_cast<int>(dev.friendlyName.size()),
                                s.data(), len, nullptr, nullptr);
            names.push_back(std::move(s));
        } else {
            names.push_back("Unknown Device");
        }
    }
    return names;
}

int WasapiDeviceList::FindByName(const std::wstring& name) const {
    if (name.empty() || name == L"default") return 0;
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (ContainsCI(devices_[i].friendlyName, name) ||
            devices_[i].id == name) {
            return static_cast<int>(i + 1);
        }
    }
    return 0;
}

int WasapiDeviceList::DefaultIndex() const {
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].isDefault) return static_cast<int>(i + 1);
    }
    return 0;
}

} // namespace svms::cfg
