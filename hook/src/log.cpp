#include "log.h"
#include <windows.h>
#include <stdarg.h>
#include <share.h>
#include <stdio.h>

namespace {
FILE* g_log = nullptr;
CRITICAL_SECTION g_lock;
bool g_lock_init = false;
}  // namespace

void tlog_open(const char* path) {
    if (!g_lock_init) {
        InitializeCriticalSection(&g_lock);
        g_lock_init = true;
    }
    // Keep one previous log; start fresh once the current one passes 5 MB.
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &info) && (info.nFileSizeHigh || info.nFileSizeLow > 5u << 20)) {
        char old[MAX_PATH];
        sprintf_s(old, "%s.1", path);
        MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);
    }
    g_log = _fsopen(path, "a", _SH_DENYNO);  // shared: readable while the game runs
}

void tlog(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
}
