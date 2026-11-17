#include "apo/ApoLog.h"

#include <windows.h>

#include <cstdarg>
#include <cwchar>
#include <mutex>

namespace vl {

namespace {

constexpr LONGLONG kMaxLogBytes = 256 * 1024;
std::mutex g_logMutex;

}  // namespace

void ApoLog(const wchar_t* format, ...) {
    wchar_t message[512];
    va_list args;
    va_start(args, format);
    vswprintf_s(message, format, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t line[640];
    const int length = swprintf_s(line, L"%02u:%02u:%02u.%03u [%lu] %s\r\n", now.wHour, now.wMinute, now.wSecond,
                                  now.wMilliseconds, GetCurrentProcessId(), message);
    char utf8[1400];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, length, utf8, sizeof(utf8), nullptr, nullptr);

    wchar_t path[MAX_PATH];
    const DWORD dirLength = GetTempPathW(MAX_PATH, path);
    if (dirLength == 0 || dirLength > MAX_PATH - 20) {
        return;
    }
    wcscat_s(path, L"VolumeLiftApo.log");

    std::lock_guard<std::mutex> lock(g_logMutex);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > kMaxLogBytes) {
        // Start over rather than grow without bound.
        CloseHandle(file);
        file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
    }
    DWORD written = 0;
    WriteFile(file, utf8, static_cast<DWORD>(bytes), &written, nullptr);
    CloseHandle(file);
}

}  // namespace vl
