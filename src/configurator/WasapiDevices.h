#ifndef SVMS_CONFIGURATOR_WASAPIDEVICES_H
#define SVMS_CONFIGURATOR_WASAPIDEVICES_H

#include <string>
#include <vector>

namespace svms::cfg {

struct AudioDevice {
    std::wstring id;
    std::wstring friendlyName;
    bool isDefault = false;
};

class WasapiDeviceList {
public:
    WasapiDeviceList();
    ~WasapiDeviceList();

    void Enumerate();
    const std::vector<AudioDevice>& Devices() const { return devices_; }
    std::vector<std::string> FriendlyNames() const;
    int FindByName(const std::wstring& name) const;
    int DefaultIndex() const;

private:
    std::vector<AudioDevice> devices_;
};

} // namespace svms::cfg

#endif
