#include "Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mxb {
namespace host {
namespace {

char g_path[MAX_PATH] = {};
bool g_ready = false;

}  // namespace

void LogInit() {
    // Sit next to the DLL that was injected, not next to the game exe.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LogInit), &self);

    char dllPath[MAX_PATH] = {};
    if (self && GetModuleFileNameA(self, dllPath, MAX_PATH)) {
        char* dot = strrchr(dllPath, '.');
        if (dot) *dot = '\0';
        _snprintf_s(g_path, sizeof(g_path), _TRUNCATE, "%s.log", dllPath);
    } else {
        char tmp[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmp);
        _snprintf_s(g_path, sizeof(g_path), _TRUNCATE, "%smxb_physics.log", tmp);
    }

    // Truncate on load so each session starts clean.
    if (FILE* f = nullptr; fopen_s(&f, g_path, "w") == 0 && f) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        fprintf(f, "mxb_physics log  %04d-%02d-%02d %02d:%02d:%02d\n", st.wYear, st.wMonth,
                st.wDay, st.wHour, st.wMinute, st.wSecond);
        fclose(f);
    }
    g_ready = true;
}

void Logf(const char* fmt, ...) {
    if (!g_ready) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "a") != 0 || !f) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

bool LogDiagnostic(const char* text, char* savedPath, std::size_t pathCapacity) {
    if (!g_ready) { SetLastError(ERROR_NOT_READY); return false; }
    if (!text || !savedPath || !pathCapacity) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    char path[MAX_PATH] = {};
    const size_t length = std::strlen(g_path);
    if (length < 4) { SetLastError(ERROR_INVALID_NAME); return false; }
    const int n = std::snprintf(path, sizeof(path), "%.*s.ban-diagnostics.log",
                                static_cast<int>(length - 4), g_path);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(path)) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD bytes = static_cast<DWORD>(std::strlen(text));
    DWORD written = 0;
    const bool writeSucceeded = WriteFile(file, text, bytes, &written, nullptr) != FALSE;
    const bool okay = writeSucceeded && written == bytes;
    const DWORD failure = writeSucceeded ? ERROR_WRITE_FAULT : GetLastError();
    CloseHandle(file);
    if (!okay) { SetLastError(failure); return false; }
    std::snprintf(savedPath, pathCapacity, "%s", path);
    Logf("%s", text);
    return true;
}

}  // namespace host
}  // namespace mxb
