// Live status the APO publishes from inside the audio engine and the app displays.
// Lives in a named shared-memory section; fields are 32/64-bit aligned so plain
// stores are atomic and the real-time thread never locks.
#pragma once

#include <windows.h>

namespace vl {

inline constexpr wchar_t kStatusSectionName[] = L"Global\\VolumeLiftStatus";
inline constexpr LONG kStatusMagic = 0x564c5354;  // "VLST"
inline constexpr LONG kStatusVersion = 1;

enum SettingsState : LONG {
    kSettingsNotRead = 0,
    kSettingsOk = 1,
    kSettingsOpenFailed = 2,
};

struct EngineStatus {
    volatile LONG magic;
    volatile LONG version;
    volatile LONG lockedInstances;   // APO instances currently processing a stream
    volatile LONG lockAttempts;
    volatile LONG lockFailures;
    volatile LONG lastLockResult;    // HRESULT of the most recent LockForProcess
    volatile LONG channels;
    volatile LONG sampleRate;
    volatile LONG settingsState;     // SettingsState
    volatile LONG settingsError;     // Win32 error when settingsState == kSettingsOpenFailed
    volatile LONG appliedGainTenths; // gain the DSP is targeting
    volatile LONG enabled;
    volatile LONG limiterPermille;   // limiter gain x1000 (1000 = not limiting)
    volatile LONG peakPermille;      // output peak of the last block x1000
    volatile LONG lastProcessTick;   // GetTickCount() of the last processed block
    volatile LONG reserved;
    volatile LONG64 framesProcessed;
};

}  // namespace vl
