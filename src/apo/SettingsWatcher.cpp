#include "apo/SettingsWatcher.h"

#include "common/Config.h"

#include <algorithm>

namespace vl {

namespace {

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

}  // namespace

SettingsWatcher::SettingsWatcher() : gainTenthsDb_(static_cast<int>(kDefaultGainTenthsDb)) {}

SettingsWatcher::~SettingsWatcher() { Stop(); }

void SettingsWatcher::Start() {
    if (thread_.joinable()) {
        return;
    }
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_) {
        return;
    }
    thread_ = std::thread([this] { Run(); });
}

void SettingsWatcher::Stop() {
    if (thread_.joinable()) {
        SetEvent(stop_);
        thread_.join();
    }
    if (stop_) {
        CloseHandle(stop_);
        stop_ = nullptr;
    }
}

void SettingsWatcher::Read(HKEY key) {
    const DWORD gain = std::min(ReadDword(key, kGainValue, kDefaultGainTenthsDb), kMaxGainTenthsDb);
    gainTenthsDb_.store(static_cast<int>(gain), std::memory_order_relaxed);
    limiter_.store(ReadDword(key, kLimiterValue, 1) != 0, std::memory_order_relaxed);
    enabled_.store(ReadDword(key, kEnabledValue, 1) != 0, std::memory_order_relaxed);
}

void SettingsWatcher::Run() {
    HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!changed) {
        return;
    }
    HKEY key = nullptr;
    for (;;) {
        if (!key && RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, KEY_READ | KEY_NOTIFY, &key) != ERROR_SUCCESS) {
            key = nullptr;
            // The key may not exist yet; check again shortly.
            if (WaitForSingleObject(stop_, 2000) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        Read(key);
        if (RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE) != ERROR_SUCCESS) {
            RegCloseKey(key);
            key = nullptr;
            continue;
        }

        HANDLE handles[] = {stop_, changed};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
            break;
        }
    }
    if (key) {
        RegCloseKey(key);
    }
    CloseHandle(changed);
}

}  // namespace vl
