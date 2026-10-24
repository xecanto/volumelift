// VolumeLift control app: a small window and tray icon for adjusting the boost,
// plus the --install / --uninstall entry points used for elevation.
#include "app/Installer.h"
#include "common/Config.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cmath>
#include <string>

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr wchar_t kWindowClass[] = L"VolumeLiftWindow";
constexpr wchar_t kRunKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr int kSliderStepTenths = 5;  // slider moves in 0.5 dB steps

enum ControlId : int {
    kSlider = 100,
    kValueLabel,
    kLimiterBox,
    kEnabledBox,
    kStartupBox,
    kStatusLabel,
    kDriverButton,
    kMenuOpen,
    kMenuEnabled,
    kMenuExit,
};

struct App {
    HWND window = nullptr;
    HWND slider = nullptr;
    HWND value = nullptr;
    HWND limiter = nullptr;
    HWND enabled = nullptr;
    HWND startup = nullptr;
    HWND status = nullptr;
    HWND driverButton = nullptr;
    HFONT font = nullptr;
    NOTIFYICONDATAW tray{};
    bool settingsWritable = false;
} g_app;

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring ExeDirectory() {
    std::wstring path = ExePath();
    return path.substr(0, path.find_last_of(L'\\'));
}

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, vl::kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) !=
        ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

bool WriteSetting(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, vl::kSettingsKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const bool ok = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) ==
                    ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool StartsWithWindows() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"VolumeLift", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

void SetStartsWithWindows(bool on) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    if (on) {
        const std::wstring command = L"\"" + ExePath() + L"\" --tray";
        RegSetValueExW(key, L"VolumeLift", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"VolumeLift");
    }
    RegCloseKey(key);
}

int Scale(int pixels) {
    HDC screen = GetDC(nullptr);
    const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    return MulDiv(pixels, dpi, 96);
}

void UpdateValueLabel(DWORD tenths) {
    const double db = tenths / 10.0;
    wchar_t text[64];
    swprintf_s(text, L"+%.1f dB   (%.2fx louder)", db, std::pow(10.0, db / 20.0));
    SetWindowTextW(g_app.value, text);
}

void UpdateTrayTip() {
    wchar_t tip[64];
    const bool on = ReadSetting(vl::kEnabledValue, 1) != 0;
    swprintf_s(tip, L"VolumeLift: %s +%.1f dB", on ? L"on," : L"off,", ReadSetting(vl::kGainValue, 0) / 10.0);
    wcscpy_s(g_app.tray.szTip, tip);
    g_app.tray.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_app.tray);
}

void RefreshState() {
    const bool installed = vl::IsDriverInstalled();
    const DWORD gain = ReadSetting(vl::kGainValue, vl::kDefaultGainTenthsDb);
    g_app.settingsWritable = installed && WriteSetting(vl::kGainValue, gain);

    SendMessageW(g_app.slider, TBM_SETPOS, TRUE, gain / kSliderStepTenths);
    UpdateValueLabel(gain);
    Button_SetCheck(g_app.limiter, ReadSetting(vl::kLimiterValue, 1) ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_app.enabled, ReadSetting(vl::kEnabledValue, 1) ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_app.startup, StartsWithWindows() ? BST_CHECKED : BST_UNCHECKED);

    for (HWND control : {g_app.slider, g_app.limiter, g_app.enabled}) {
        EnableWindow(control, g_app.settingsWritable);
    }
    SetWindowTextW(g_app.status, !installed                 ? L"Audio driver not installed."
                                 : g_app.settingsWritable ? L"Audio driver installed and active."
                                                          : L"Driver installed, but settings are read-only. Reinstall.");
    SetWindowTextW(g_app.driverButton, installed ? L"Uninstall driver" : L"Install driver");
    UpdateTrayTip();
}

// Runs this exe elevated with one argument and waits for it to finish.
bool RunElevated(const wchar_t* argument) {
    const std::wstring exe = ExePath();
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = g_app.window;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = argument;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        return false;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code == 0;
}

void OnDriverButton() {
    const bool installed = vl::IsDriverInstalled();
    if (installed &&
        MessageBoxW(g_app.window,
                    L"Remove the VolumeLift audio driver and restore your devices' original audio effects?",
                    L"VolumeLift", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    if (!installed &&
        MessageBoxW(g_app.window,
                    L"Installing registers VolumeLift as an audio effect on every playback device and briefly "
                    L"restarts Windows Audio. Your devices' existing endpoint effects are backed up and restored "
                    L"on uninstall.\n\nContinue?",
                    L"VolumeLift", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    RunElevated(installed ? L"--uninstall" : L"--install");
    RefreshState();
}

void ShowWindowFromTray() {
    ShowWindow(g_app.window, SW_SHOWNORMAL);
    SetForegroundWindow(g_app.window);
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    const bool on = ReadSetting(vl::kEnabledValue, 1) != 0;
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open VolumeLift");
    AppendMenuW(menu, MF_STRING | (on ? MF_CHECKED : 0) | (g_app.settingsWritable ? 0 : MF_GRAYED), kMenuEnabled,
                L"Boost enabled");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(g_app.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_app.window, nullptr);
    DestroyMenu(menu);
}

HWND AddControl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, Scale(x), Scale(y), Scale(w), Scale(h),
                                   g_app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.font), TRUE);
    return control;
}

void CreateControls() {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    g_app.font = CreateFontIndirectW(&metrics.lfMessageFont);

    AddControl(L"STATIC", L"Boost", 0, 16, 14, 300, 20, 0);
    g_app.slider = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 10, 36, 320, 32, kSlider);
    SendMessageW(g_app.slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, vl::kMaxGainTenthsDb / kSliderStepTenths));
    SendMessageW(g_app.slider, TBM_SETTICFREQ, 12, 0);
    SendMessageW(g_app.slider, TBM_SETPAGESIZE, 0, 2);
    g_app.value = AddControl(L"STATIC", L"", 0, 16, 72, 300, 20, kValueLabel);
    g_app.limiter = AddControl(L"BUTTON", L"Limiter (prevents distortion)", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 100, 300,
                               22, kLimiterBox);
    g_app.enabled = AddControl(L"BUTTON", L"Boost enabled", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 124, 300, 22, kEnabledBox);
    g_app.startup = AddControl(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 148, 300, 22,
                               kStartupBox);
    g_app.status = AddControl(L"STATIC", L"", 0, 16, 182, 310, 20, kStatusLabel);
    g_app.driverButton = AddControl(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 16, 208, 140, 28, kDriverButton);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_app.window = window;
        CreateControls();
        g_app.tray.cbSize = sizeof(g_app.tray);
        g_app.tray.hWnd = window;
        g_app.tray.uID = kTrayId;
        g_app.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_app.tray.uCallbackMessage = kTrayMessage;
        g_app.tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(g_app.tray.szTip, L"VolumeLift");
        Shell_NotifyIconW(NIM_ADD, &g_app.tray);
        RefreshState();
        return 0;

    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == g_app.slider) {
            const DWORD tenths = static_cast<DWORD>(SendMessageW(g_app.slider, TBM_GETPOS, 0, 0)) * kSliderStepTenths;
            UpdateValueLabel(tenths);
            WriteSetting(vl::kGainValue, tenths);
            UpdateTrayTip();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kLimiterBox:
            WriteSetting(vl::kLimiterValue, Button_GetCheck(g_app.limiter) == BST_CHECKED);
            return 0;
        case kEnabledBox:
            WriteSetting(vl::kEnabledValue, Button_GetCheck(g_app.enabled) == BST_CHECKED);
            UpdateTrayTip();
            return 0;
        case kStartupBox:
            SetStartsWithWindows(Button_GetCheck(g_app.startup) == BST_CHECKED);
            return 0;
        case kDriverButton:
            OnDriverButton();
            return 0;
        case kMenuOpen:
            ShowWindowFromTray();
            return 0;
        case kMenuEnabled:
            WriteSetting(vl::kEnabledValue, ReadSetting(vl::kEnabledValue, 1) ? 0 : 1);
            RefreshState();
            return 0;
        case kMenuExit:
            DestroyWindow(window);
            return 0;
        }
        break;

    case kTrayMessage:
        if (LOWORD(lParam) == WM_LBUTTONUP) {
            IsWindowVisible(window) ? ShowWindow(window, SW_HIDE) : ShowWindowFromTray();
        } else if (LOWORD(lParam) == WM_RBUTTONUP) {
            ShowTrayMenu();
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);  // Keep running in the tray.
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_app.tray);
        DeleteObject(g_app.font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int RunInstaller(bool install) {
    if (!vl::IsElevated()) {
        return RunElevated(install ? L"--install" : L"--uninstall") ? 0 : 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const vl::InstallResult result = install ? vl::Install(ExeDirectory()) : vl::Uninstall();
    CoUninitialize();
    MessageBoxW(nullptr, result.log.c_str(), result.ok ? L"VolumeLift" : L"VolumeLift - errors",
                MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    return result.ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
    const std::wstring args = commandLine ? commandLine : L"";
    if (args.find(L"--install") != std::wstring::npos) {
        return RunInstaller(true);
    }
    if (args.find(L"--uninstall") != std::wstring::npos) {
        return RunInstaller(false);
    }

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\VolumeLiftApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            ShowWindow(existing, SW_SHOWNORMAL);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    RECT rect{0, 0, Scale(344), Scale(252)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);
    HWND window = CreateWindowExW(0, kWindowClass, L"VolumeLift", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!window) {
        return 1;
    }
    if (args.find(L"--tray") == std::wstring::npos) {
        ShowWindow(window, show);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CloseHandle(single);
    return static_cast<int>(msg.wParam);
}
