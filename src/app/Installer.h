// Installs the APO system-wide and attaches it to every active playback device.
// Every registry value or permission it changes is backed up and restored on uninstall.
#pragma once

#include <string>

namespace vl {

struct InstallResult {
    bool ok = false;
    std::wstring log;
};

std::wstring InstallDirectory();
bool IsDriverInstalled();
bool IsElevated();

// Both require administrator rights.
InstallResult Install(const std::wstring& sourceDirectory);
InstallResult Uninstall();

}  // namespace vl
