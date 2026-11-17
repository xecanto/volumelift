#include "app/Installer.h"

#include "common/Config.h"

#include <windows.h>
#include <aclapi.h>
#include <mmdeviceapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <cstring>
#include <vector>

namespace vl {

namespace {

// Endpoint FX property keys, as stored under MMDevices\Audio\Render\{id}\FxProperties.
constexpr wchar_t kEndpointEffect[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";
constexpr wchar_t kCompositeEndpointEffect[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15";
// PKEY_AudioEndpoint_Disable_SysFx, under ...\{id}\Properties. 0 = enhancements enabled.
constexpr wchar_t kDisableSysFx[] = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";

constexpr wchar_t kRenderRoot[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";
constexpr wchar_t kAudioPolicyKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio";
constexpr wchar_t kDisableProtectedAudioDG[] = L"DisableProtectedAudioDG";
constexpr wchar_t kBackupKey[] = L"SOFTWARE\\VolumeLift\\Backup";
constexpr wchar_t kAbsentList[] = L"__absent";
constexpr wchar_t kSddlValue[] = L"__sddl";

class Log {
public:
    void Line(const std::wstring& text) { text_ += text + L"\r\n"; }
    void Error(const std::wstring& what, DWORD code) {
        wchar_t* message = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
        Line(L"ERROR: " + what + L" (" + std::to_wstring(code) + L") " + (message ? message : L""));
        if (message) {
            LocalFree(message);
        }
        failed_ = true;
    }
    bool Failed() const { return failed_; }
    const std::wstring& Text() const { return text_; }

private:
    std::wstring text_;
    bool failed_ = false;
};

struct RegValue {
    DWORD type = REG_NONE;
    std::vector<BYTE> data;
};

bool ReadValue(HKEY key, const wchar_t* name, RegValue& out) {
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &out.type, nullptr, &size) != ERROR_SUCCESS) {
        return false;
    }
    out.data.resize(size);
    return RegQueryValueExW(key, name, nullptr, &out.type, out.data.data(), &size) == ERROR_SUCCESS;
}

LSTATUS WriteValue(HKEY key, const wchar_t* name, const RegValue& value) {
    return RegSetValueExW(key, name, 0, value.type, value.data.data(), static_cast<DWORD>(value.data.size()));
}

RegValue StringValue(const std::wstring& text, DWORD type = REG_SZ) {
    RegValue value;
    value.type = type;
    const size_t chars = text.size() + (type == REG_MULTI_SZ ? 2 : 1);
    value.data.resize(chars * sizeof(wchar_t));
    memcpy(value.data.data(), text.c_str(), text.size() * sizeof(wchar_t));
    return value;
}

RegValue DwordValue(DWORD number) {
    RegValue value;
    value.type = REG_DWORD;
    value.data.resize(sizeof(DWORD));
    memcpy(value.data.data(), &number, sizeof(DWORD));
    return value;
}

std::vector<std::wstring> ReadMultiString(HKEY key, const wchar_t* name) {
    std::vector<std::wstring> items;
    RegValue value;
    if (!ReadValue(key, name, value) || value.type != REG_MULTI_SZ) {
        return items;
    }
    const auto* p = reinterpret_cast<const wchar_t*>(value.data.data());
    const auto* end = p + value.data.size() / sizeof(wchar_t);
    while (p < end && *p) {
        items.emplace_back(p);
        p += items.back().size() + 1;
    }
    return items;
}

void WriteMultiString(HKEY key, const wchar_t* name, const std::vector<std::wstring>& items) {
    std::wstring joined;
    for (const auto& item : items) {
        joined += item;
        joined.push_back(L'\0');
    }
    WriteValue(key, name, StringValue(joined, REG_MULTI_SZ));
}

bool EnablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

std::vector<BYTE> WellKnownSid(WELL_KNOWN_SID_TYPE type) {
    std::vector<BYTE> sid(SECURITY_MAX_SID_SIZE);
    DWORD size = static_cast<DWORD>(sid.size());
    if (!CreateWellKnownSid(type, nullptr, sid.data(), &size)) {
        return {};
    }
    sid.resize(size);
    return sid;
}

// "MACHINE\..." form used by the *NamedSecurityInfo APIs for HKLM keys.
std::wstring SecurityPath(const std::wstring& subkey) { return L"MACHINE\\" + subkey; }

std::wstring ReadSddl(const std::wstring& subkey) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    std::wstring path = SecurityPath(subkey);
    if (GetNamedSecurityInfoW(path.c_str(), SE_REGISTRY_KEY,
                              OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, nullptr, nullptr, &descriptor) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t* sddl = nullptr;
    std::wstring result;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(
            descriptor, SDDL_REVISION_1,
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &sddl, nullptr)) {
        result = sddl;
        LocalFree(sddl);
    }
    LocalFree(descriptor);
    return result;
}

void RestoreSddl(const std::wstring& subkey, const std::wstring& sddl, Log& log) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        log.Error(L"Parse saved permissions for " + subkey, GetLastError());
        return;
    }
    PSID owner = nullptr, group = nullptr;
    PACL dacl = nullptr;
    BOOL defaulted = FALSE, present = FALSE;
    GetSecurityDescriptorOwner(descriptor, &owner, &defaulted);
    GetSecurityDescriptorGroup(descriptor, &group, &defaulted);
    GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    GetSecurityDescriptorControl(descriptor, &control, &revision);
    SECURITY_INFORMATION info = DACL_SECURITY_INFORMATION | (owner ? OWNER_SECURITY_INFORMATION : 0) |
                                (group ? GROUP_SECURITY_INFORMATION : 0) |
                                ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                                               : UNPROTECTED_DACL_SECURITY_INFORMATION);
    std::wstring path = SecurityPath(subkey);
    const DWORD status = SetNamedSecurityInfoW(path.data(), SE_REGISTRY_KEY, info, owner, group, dacl, nullptr);
    if (status != ERROR_SUCCESS) {
        log.Error(L"Restore permissions on " + subkey, status);
    }
    LocalFree(descriptor);
}

// Grants `sid` the given access on an HKLM key, merging with its existing DACL.
DWORD GrantAccess(const std::wstring& subkey, std::vector<BYTE>& sid, DWORD access, DWORD inheritance) {
    std::wstring path = SecurityPath(subkey);
    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD status = GetNamedSecurityInfoW(path.c_str(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                         &oldDacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        return status;
    }
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = access;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = inheritance;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data());
    PACL newDacl = nullptr;
    status = SetEntriesInAclW(1, &entry, oldDacl, &newDacl);
    if (status == ERROR_SUCCESS) {
        status = SetNamedSecurityInfoW(path.data(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                       newDacl, nullptr);
        LocalFree(newDacl);
    }
    LocalFree(descriptor);
    return status;
}

// Audio endpoint keys are owned by the system; make Administrators the owner with full control.
bool UnlockKey(const std::wstring& subkey, Log& log) {
    auto admins = WellKnownSid(WinBuiltinAdministratorsSid);
    std::wstring path = SecurityPath(subkey);
    DWORD status = SetNamedSecurityInfoW(path.data(), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION, admins.data(),
                                         nullptr, nullptr, nullptr);
    if (status != ERROR_SUCCESS) {
        log.Error(L"Take ownership of " + subkey, status);
        return false;
    }
    status = GrantAccess(subkey, admins, KEY_ALL_ACCESS, NO_INHERITANCE);
    if (status != ERROR_SUCCESS) {
        log.Error(L"Grant access to " + subkey, status);
        return false;
    }
    return true;
}

// Saves the listed values (or their absence) and the key's permissions, once.
void BackUpKey(const std::wstring& subkey, const std::wstring& backupSubkey,
               const std::vector<const wchar_t*>& names) {
    HKEY backup = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, backupSubkey.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &backup,
                        &disposition) != ERROR_SUCCESS) {
        return;
    }
    if (disposition == REG_OPENED_EXISTING_KEY) {
        RegCloseKey(backup);  // Keep the backup from the first install.
        return;
    }
    WriteValue(backup, kSddlValue, StringValue(ReadSddl(subkey)));
    HKEY source = nullptr;
    std::vector<std::wstring> absent;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ, &source) == ERROR_SUCCESS) {
        for (const wchar_t* name : names) {
            RegValue value;
            if (ReadValue(source, name, value)) {
                WriteValue(backup, name, value);
            } else {
                absent.emplace_back(name);
            }
        }
        RegCloseKey(source);
    } else {
        for (const wchar_t* name : names) {
            absent.emplace_back(name);
        }
    }
    WriteMultiString(backup, kAbsentList, absent);
    RegCloseKey(backup);
}

void RestoreKey(const std::wstring& subkey, const std::wstring& backupSubkey, Log& log) {
    HKEY backup = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, backupSubkey.c_str(), 0, KEY_READ, &backup) != ERROR_SUCCESS) {
        return;
    }
    HKEY target = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_SET_VALUE, &target) == ERROR_SUCCESS) {
        for (const auto& name : ReadMultiString(backup, kAbsentList)) {
            RegDeleteValueW(target, name.c_str());
        }
        for (DWORD i = 0;; ++i) {
            wchar_t name[256];
            DWORD nameLength = 256;
            if (RegEnumValueW(backup, i, name, &nameLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            if (wcscmp(name, kAbsentList) == 0 || wcscmp(name, kSddlValue) == 0) {
                continue;
            }
            RegValue value;
            if (ReadValue(backup, name, value)) {
                WriteValue(target, name, value);
            }
        }
        RegCloseKey(target);
    }
    RegValue sddl;
    if (ReadValue(backup, kSddlValue, sddl) && sddl.data.size() > sizeof(wchar_t)) {
        RestoreSddl(subkey, reinterpret_cast<const wchar_t*>(sddl.data.data()), log);
    }
    RegCloseKey(backup);
    log.Line(L"Restored " + subkey);
}

struct Endpoint {
    std::wstring key;  // "{guid}" under MMDevices\Audio\Render
};

std::vector<Endpoint> ActiveRenderEndpoints(Log& log) {
    std::vector<Endpoint> endpoints;
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        log.Error(L"Create device enumerator", hr);
        return endpoints;
    }
    IMMDeviceCollection* devices = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            LPWSTR id = nullptr;
            if (SUCCEEDED(devices->Item(i, &device)) && SUCCEEDED(device->GetId(&id))) {
                // Device IDs look like "{0.0.0.00000000}.{endpoint-guid}".
                std::wstring full = id;
                const size_t dot = full.rfind(L'.');
                if (dot != std::wstring::npos) {
                    endpoints.push_back({full.substr(dot + 1)});
                }
                CoTaskMemFree(id);
            }
            if (device) {
                device->Release();
            }
        }
        devices->Release();
    }
    enumerator->Release();
    return endpoints;
}

bool ControlAudioService(bool start, Log& log) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        log.Error(L"Open service manager", GetLastError());
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, L"Audiosrv", SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        log.Error(L"Open Windows Audio service", GetLastError());
        CloseServiceHandle(manager);
        return false;
    }
    SERVICE_STATUS status{};
    bool requested = start ? StartServiceW(service, 0, nullptr) != FALSE
                           : ControlService(service, SERVICE_CONTROL_STOP, &status) != FALSE;
    const DWORD error = requested ? ERROR_SUCCESS : GetLastError();
    const DWORD wanted = start ? SERVICE_RUNNING : SERVICE_STOPPED;
    bool ok = requested || error == ERROR_SERVICE_ALREADY_RUNNING || error == ERROR_SERVICE_NOT_ACTIVE;
    if (!ok) {
        log.Error(start ? L"Start Windows Audio" : L"Stop Windows Audio", error);
    }
    for (int i = 0; ok && i < 100; ++i) {
        if (QueryServiceStatus(service, &status) && status.dwCurrentState == wanted) {
            break;
        }
        Sleep(200);
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (ok) {
        log.Line(start ? L"Windows Audio started" : L"Windows Audio stopped");
    }
    return ok;
}

using RegisterFn = HRESULT(STDAPICALLTYPE*)();

bool CallDllExport(const std::wstring& dll, const char* name, Log& log) {
    HMODULE module = LoadLibraryW(dll.c_str());
    if (!module) {
        log.Error(L"Load " + dll, GetLastError());
        return false;
    }
    auto fn = reinterpret_cast<RegisterFn>(GetProcAddress(module, name));
    const HRESULT hr = fn ? fn() : E_NOINTERFACE;
    FreeLibrary(module);
    if (FAILED(hr)) {
        log.Error(std::wstring(L"Call ") + std::wstring(name, name + strlen(name)), hr);
        return false;
    }
    return true;
}

void CreateSettingsKey(Log& log) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        log.Error(L"Create settings key", GetLastError());
        return;
    }
    RegValue existing;
    if (!ReadValue(key, kGainValue, existing)) {
        WriteValue(key, kGainValue, DwordValue(kDefaultGainTenthsDb));
    }
    if (!ReadValue(key, kLimiterValue, existing)) {
        WriteValue(key, kLimiterValue, DwordValue(1));
    }
    if (!ReadValue(key, kEnabledValue, existing)) {
        WriteValue(key, kEnabledValue, DwordValue(1));
    }
    RegCloseKey(key);

    // Let the tray app change settings without elevation. Not inherited by the backup subkey.
    auto users = WellKnownSid(WinBuiltinUsersSid);
    const DWORD status = GrantAccess(kSettingsKey, users, KEY_READ | KEY_SET_VALUE, NO_INHERITANCE);
    if (status != ERROR_SUCCESS) {
        log.Error(L"Grant users access to settings", status);
    }
}

void WriteLogFile(const std::wstring& text) {
    wchar_t* programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) {
        return;
    }
    std::wstring dir = std::wstring(programData) + L"\\VolumeLift";
    CoTaskMemFree(programData);
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE file = CreateFileW((dir + L"\\install.log").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr,
                        nullptr);
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

InstallResult Finish(Log& log) {
    WriteLogFile(log.Text());
    return {!log.Failed(), log.Text()};
}

}  // namespace

std::wstring InstallDirectory() {
    wchar_t* programFiles = nullptr;
    std::wstring dir = L"C:\\Program Files";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles))) {
        dir = programFiles;
        CoTaskMemFree(programFiles);
    }
    return dir + L"\\VolumeLift";
}

bool IsDriverInstalled() {
    HKEY key = nullptr;
    const std::wstring path = std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + kApoClsidString;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

bool IsElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                   elevation.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated;
}

InstallResult Install(const std::wstring& sourceDirectory) {
    Log log;
    log.Line(L"VolumeLift install");
    EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
    EnablePrivilege(SE_RESTORE_NAME);
    EnablePrivilege(SE_BACKUP_NAME);

    const auto endpoints = ActiveRenderEndpoints(log);
    if (endpoints.empty()) {
        log.Line(L"ERROR: no active playback devices found");
        log.Error(L"Find playback devices", ERROR_NOT_FOUND);
        return Finish(log);
    }

    ControlAudioService(false, log);

    const std::wstring dir = InstallDirectory();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring dll = dir + L"\\VolumeLiftApo.dll";
    for (const wchar_t* file : {L"VolumeLiftApo.dll", L"VolumeLift.exe"}) {
        const std::wstring from = sourceDirectory + L"\\" + file;
        const std::wstring to = dir + L"\\" + file;
        if (_wcsicmp(from.c_str(), to.c_str()) != 0 && !CopyFileW(from.c_str(), to.c_str(), FALSE)) {
            log.Error(L"Copy " + from, GetLastError());
        }
    }
    log.Line(L"Installed files to " + dir);

    if (CallDllExport(dll, "DllRegisterServer", log)) {
        log.Line(L"Registered APO " + std::wstring(kApoClsidString));
    }
    CreateSettingsKey(log);

    // Unsigned third-party APOs need the audio engine's protected mode off.
    BackUpKey(kAudioPolicyKey, std::wstring(kBackupKey) + L"\\AudioPolicy", {kDisableProtectedAudioDG});
    HKEY policy = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kAudioPolicyKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &policy,
                        nullptr) == ERROR_SUCCESS) {
        WriteValue(policy, kDisableProtectedAudioDG, DwordValue(1));
        RegCloseKey(policy);
    }

    for (const auto& endpoint : endpoints) {
        const std::wstring base = std::wstring(kRenderRoot) + endpoint.key;
        const std::wstring fx = base + L"\\FxProperties";
        const std::wstring props = base + L"\\Properties";
        const std::wstring backup = std::wstring(kBackupKey) + L"\\" + endpoint.key;

        BackUpKey(fx, backup + L"\\FxProperties", {kEndpointEffect, kCompositeEndpointEffect});
        BackUpKey(props, backup + L"\\Properties", {kDisableSysFx});

        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fx.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
            // Some devices have no FxProperties yet; create it under an unlocked endpoint key.
            BackUpKey(base, backup + L"\\Endpoint", {});
            if (UnlockKey(base, log)) {
                RegCreateKeyExW(HKEY_LOCAL_MACHINE, fx.c_str(), 0, nullptr, 0, KEY_READ, nullptr, &key, nullptr);
            }
        }
        if (key) {
            RegCloseKey(key);
        }
        if (!UnlockKey(fx, log) || RegOpenKeyExW(HKEY_LOCAL_MACHINE, fx.c_str(), 0, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS) {
            log.Line(L"Skipped device " + endpoint.key);
            continue;
        }
        // Keep the device's own endpoint effects and run VolumeLift last, so the limiter sees the final signal.
        std::vector<std::wstring> chain = ReadMultiString(key, kCompositeEndpointEffect);
        RegValue legacy;
        if (chain.empty() && ReadValue(key, kEndpointEffect, legacy) && legacy.type == REG_SZ) {
            const std::wstring existing = reinterpret_cast<const wchar_t*>(legacy.data.data());
            if (!existing.empty() && _wcsicmp(existing.c_str(), L"{00000000-0000-0000-0000-000000000000}") != 0) {
                chain.push_back(existing);
            }
        }
        std::erase_if(chain, [](const std::wstring& clsid) { return _wcsicmp(clsid.c_str(), kApoClsidString) == 0; });
        chain.emplace_back(kApoClsidString);
        WriteMultiString(key, kCompositeEndpointEffect, chain);
        if (chain.size() == 1) {
            WriteValue(key, kEndpointEffect, StringValue(kApoClsidString));
        }
        RegCloseKey(key);

        if (UnlockKey(props, log) &&
            RegOpenKeyExW(HKEY_LOCAL_MACHINE, props.c_str(), 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS) {
            RegValue disabled;
            if (ReadValue(key, kDisableSysFx, disabled)) {
                WriteValue(key, kDisableSysFx, DwordValue(0));
            }
            RegCloseKey(key);
        }
        log.Line(L"Attached to device " + endpoint.key);
    }

    ControlAudioService(true, log);
    log.Line(log.Failed() ? L"Finished with errors." : L"Done.");
    return Finish(log);
}

InstallResult Uninstall() {
    Log log;
    log.Line(L"VolumeLift uninstall");
    EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
    EnablePrivilege(SE_RESTORE_NAME);
    EnablePrivilege(SE_BACKUP_NAME);

    ControlAudioService(false, log);

    HKEY backups = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kBackupKey, 0, KEY_READ, &backups) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t name[256];
            DWORD length = 256;
            if (RegEnumKeyExW(backups, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            const std::wstring backup = std::wstring(kBackupKey) + L"\\" + name;
            if (wcscmp(name, L"AudioPolicy") == 0) {
                RestoreKey(kAudioPolicyKey, backup, log);
                continue;
            }
            const std::wstring base = std::wstring(kRenderRoot) + name;
            RestoreKey(base + L"\\FxProperties", backup + L"\\FxProperties", log);
            RestoreKey(base + L"\\Properties", backup + L"\\Properties", log);
            RestoreKey(base, backup + L"\\Endpoint", log);
        }
        RegCloseKey(backups);
    }

    const std::wstring dir = InstallDirectory();
    const std::wstring dll = dir + L"\\VolumeLiftApo.dll";
    if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CallDllExport(dll, "DllUnregisterServer", log);
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kSettingsKey);

    ControlAudioService(true, log);

    for (const wchar_t* file : {L"VolumeLiftApo.dll", L"VolumeLift.exe"}) {
        const std::wstring path = dir + L"\\" + file;
        if (!DeleteFileW(path.c_str())) {
            MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    RemoveDirectoryW(dir.c_str());
    log.Line(log.Failed() ? L"Finished with errors." : L"Done.");
    return Finish(log);
}

}  // namespace vl
