// Watches the VolumeLift settings key and publishes values through atomics,
// so the real-time audio thread never touches the registry.
#pragma once

#include <windows.h>

#include <atomic>
#include <thread>

#include "common/EngineStatus.h"

namespace vl {

class SettingsWatcher {
public:
    SettingsWatcher();
    ~SettingsWatcher();
    SettingsWatcher(const SettingsWatcher&) = delete;
    SettingsWatcher& operator=(const SettingsWatcher&) = delete;

    // `status` (may be null) receives whether the settings key could be read.
    void Start(EngineStatus* status);
    void Stop();

    float GainDb() const { return gainTenthsDb_.load(std::memory_order_relaxed) / 10.0f; }
    bool Limiter() const { return limiter_.load(std::memory_order_relaxed); }
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

private:
    void Run();
    void Read(HKEY key);

    std::atomic<int> gainTenthsDb_;
    std::atomic<bool> limiter_{true};
    std::atomic<bool> enabled_{true};
    HANDLE stop_ = nullptr;
    EngineStatus* status_ = nullptr;
    std::thread thread_;
};

}  // namespace vl
