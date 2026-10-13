#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "log.h"

static CRITICAL_SECTION g_cs;
static int g_init, g_level = 1;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static LONG g_refs;

static int reg_read(const char *name, char *buf, DWORD cb)
{
    HKEY k; DWORD type, sz = cb; int ok = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\FFmpegVfW", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, name, NULL, &type, (BYTE *)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) ok = 1;
        RegCloseKey(k);
    }
    return ok;
}

static int cfg_get(const char *name, char *buf, DWORD cb)
{
    DWORD n = GetEnvironmentVariableA(name, buf, cb);
    if (n > 0 && n < cb) return 1;
    return reg_read(name, buf, cb);
}

int ffvfw_cfg_string(const char *name, char *buf, unsigned cb) { return cfg_get(name, buf, cb); }

void ffvfw_log_init(void)
{
    char buf[MAX_PATH];
    if (InterlockedIncrement(&g_refs) != 1) return;
    InitializeCriticalSection(&g_cs);
    g_init = 1;
    if (cfg_get("FFVFW_LOGLEVEL", buf, sizeof buf)) g_level = atoi(buf);
    if (cfg_get("FFVFW_LOG", buf, sizeof buf))
        g_file = CreateFileA(buf, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void ffvfw_log_shutdown(void)
{
    if (InterlockedDecrement(&g_refs) != 0) return;
    g_init = 0;
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    DeleteCriticalSection(&g_cs);
}

int ffvfw_log_level(void) { return g_level; }

void ffvfw_log(int level, const char *fmt, ...)
{
    char line[1024]; int n; va_list ap; SYSTEMTIME st;
    if (!g_init || level > g_level) return;
    GetLocalTime(&st);
    n = _snprintf(line, 200, "%02d:%02d:%02d.%03d [ffvfw p%lu t%lu] ", st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId());
    va_start(ap, fmt);
    n += _vsnprintf(line + n, sizeof line - 3 - n, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof line - 3) n = sizeof line - 3;
    line[n++] = '\r'; line[n++] = '\n'; line[n] = 0;
    EnterCriticalSection(&g_cs);
    OutputDebugStringA(line);
    if (g_file != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(g_file, line, n, &w, NULL); }
    LeaveCriticalSection(&g_cs);
}
