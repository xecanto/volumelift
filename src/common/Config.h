// Identifiers and registry locations shared by the APO and the control app.
#pragma once

#include <windows.h>

namespace vl {

// COM class ID of the VolumeLift audio processing object.
inline constexpr GUID kApoClsid = {0x8ed445f5, 0xf069, 0x45eb, {0x9c, 0x67, 0x6e, 0x15, 0x08, 0x4b, 0x22, 0x5b}};
inline constexpr wchar_t kApoClsidString[] = L"{8ED445F5-F069-45EB-9C67-6E15084B225B}";
inline constexpr wchar_t kApoFriendlyName[] = L"VolumeLift Boost";
inline constexpr wchar_t kVersion[] = L"0.1.1";
inline constexpr wchar_t kProjectUrl[] = L"https://github.com/xecanto/volumelift";

// Settings live in HKLM so the audio engine (running as LocalService) can read them.
// The installer grants interactive users write access to this key.
inline constexpr wchar_t kSettingsKey[] = L"SOFTWARE\\VolumeLift";
inline constexpr wchar_t kGainValue[] = L"GainTenthsDb";  // DWORD, 0..kMaxGainTenthsDb
inline constexpr wchar_t kLimiterValue[] = L"Limiter";    // DWORD, 0 or 1
inline constexpr wchar_t kEnabledValue[] = L"Enabled";    // DWORD, 0 or 1

inline constexpr DWORD kMaxGainTenthsDb = 240;  // +24 dB
inline constexpr DWORD kDefaultGainTenthsDb = 60;

}  // namespace vl
