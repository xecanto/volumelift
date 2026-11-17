// COM server entry points for the VolumeLift APO DLL.
#include "apo/VolumeLiftApo.h"

#include "apo/ApoLog.h"
#include "common/Config.h"

#include <new>
#include <string>

namespace {

HMODULE g_module = nullptr;

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *object = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    // The factory is a static object; its lifetime is the module's.
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
        wchar_t iid[64] = {};
        StringFromGUID2(riid, iid, 64);
        vl::ApoLog(L"CreateInstance %s outer=%p", iid, outer);
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        // COM aggregation rule: an aggregated object is only handed out as its inner IUnknown.
        if (outer && riid != __uuidof(IUnknown)) {
            vl::ApoLog(L"  -> CLASS_E_NOAGGREGATION");
            return CLASS_E_NOAGGREGATION;
        }
        auto* apo = new (std::nothrow) vl::VolumeLiftApo(outer);
        if (!apo) {
            return E_OUTOFMEMORY;
        }
        IUnknown* inner = apo->Inner();
        const HRESULT hr = inner->QueryInterface(riid, object);
        inner->Release();
        vl::ApoLog(L"  -> 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    STDMETHODIMP LockServer(BOOL lock) override {
        lock ? ++vl::g_serverRefs : --vl::g_serverRefs;
        return S_OK;
    }
};

ClassFactory g_factory;

std::wstring GuidString(const GUID& guid) {
    wchar_t buffer[64] = {};
    StringFromGUID2(guid, buffer, 64);
    return buffer;
}

LSTATUS SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

LSTATUS SetDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

const std::wstring kClsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + vl::kApoClsidString;
const std::wstring kApoKey = std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + vl::kApoClsidString;

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
    if (!object) {
        return E_POINTER;
    }
    *object = nullptr;
    wchar_t requested[64] = {};
    wchar_t iid[64] = {};
    StringFromGUID2(clsid, requested, 64);
    StringFromGUID2(riid, iid, 64);
    if (clsid != vl::kApoClsid) {
        vl::ApoLog(L"DllGetClassObject %s %s -> CLASS_E_CLASSNOTAVAILABLE", requested, iid);
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    const HRESULT hr = g_factory.QueryInterface(riid, object);
    vl::ApoLog(L"DllGetClassObject %s %s -> 0x%08lX", requested, iid, static_cast<unsigned long>(hr));
    return hr;
}

STDAPI DllCanUnloadNow() { return vl::g_serverRefs.load() == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, (kClsidKey + L"\\InprocServer32").c_str(), 0, nullptr, 0,
                                     KEY_WRITE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(status);
    }
    SetString(key, nullptr, path);
    SetString(key, L"ThreadingModel", L"Both");
    RegCloseKey(key);

    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kClsidKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(status);
    }
    SetString(key, nullptr, vl::kApoFriendlyName);
    RegCloseKey(key);

    // Audio engine registration, mirroring APO_REG_PROPERTIES.
    APO_REG_PROPERTIES props;
    vl::VolumeLiftApo::FillRegistration(props);
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kApoKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(status);
    }
    SetString(key, L"FriendlyName", props.szFriendlyName);
    SetString(key, L"Copyright", props.szCopyrightInfo);
    SetDword(key, L"MajorVersion", props.u32MajorVersion);
    SetDword(key, L"MinorVersion", props.u32MinorVersion);
    SetDword(key, L"Flags", static_cast<DWORD>(props.Flags));
    SetDword(key, L"MinInputConnections", props.u32MinInputConnections);
    SetDword(key, L"MaxInputConnections", props.u32MaxInputConnections);
    SetDword(key, L"MinOutputConnections", props.u32MinOutputConnections);
    SetDword(key, L"MaxOutputConnections", props.u32MaxOutputConnections);
    SetDword(key, L"MaxInstances", props.u32MaxInstances);
    SetDword(key, L"NumAPOInterfaces", props.u32NumAPOInterfaces);
    SetString(key, L"APOInterface0", GuidString(props.iidAPOInterfaceList[0]));
    RegCloseKey(key);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kApoKey.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey.c_str());
    return S_OK;
}
