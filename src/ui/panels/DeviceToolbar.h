#pragma once

// The always-visible bar above the dockspace: device picker, package picker
// and the sampling toggles. Not a Panel -- it is drawn into a viewport side
// bar so it cannot be undocked or hidden.

#include <string>
#include <vector>

#include "app/AppContext.h"

namespace em {

class DeviceToolbar {
public:
    void draw(AppContext& context);
    void onDeviceChanged();

private:
    void drawDevicePicker(AppContext& context);
    void drawPackagePicker(AppContext& context);
    void refreshPackages(AppContext& context);

    std::vector<std::string> packages_;
    std::string packageFilter_;
    bool thirdPartyOnly_ = true;
    bool packagesLoading_ = false;
    bool packagesRequested_ = false;
    std::string packageError_;
};

}  // namespace em
