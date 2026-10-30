// Backstop for endings the game process can't log ([probe] LastRunCheck=1, the default). A
// fast-fail (int 0x29: Windows' heap and CFG checks, newer CRTs' /GS) ends the process without
// running any handler, and a freeze ended by Windows or Task Manager leaves no trace either.
// A session that ends normally writes "process exiting" last. At startup, when the previous
// session's events log doesn't end that way, a thread looks up what Windows recorded for that
// process in the Application event log - "Application Error" (1000: the exception code,
// faulting module and offset) or "Application Hang" (1002) - and appends it to that log, so
// the log of the game that crashed holds it once the game has been started again. The current
// log gets the same line. Windows writes the event a little after the crash, so the lookup is
// retried for a minute.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

typedef HANDLE(WINAPI *EvtQuery_t)(HANDLE, LPCWSTR, LPCWSTR, DWORD);
typedef BOOL(WINAPI *EvtNext_t)(HANDLE, DWORD, HANDLE *, DWORD, DWORD, PDWORD);
typedef BOOL(WINAPI *EvtRender_t)(HANDLE, HANDLE, DWORD, DWORD, PVOID, PDWORD, PDWORD);
typedef BOOL(WINAPI *EvtClose_t)(HANDLE);
#define EVT_QUERY_CHANNEL_PATH 0x1
#define EVT_QUERY_REVERSE 0x200
#define EVT_RENDER_XML 1

static WCHAR g_prevPath[MAX_PATH];
static char g_prevName[64];
static DWORD g_prevPid;
static WCHAR g_since[40];  // the previous session's start, as event-log UTC time

// Appends a line to the previous session's log and logs it in this one.
static void report(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log_event("previous launch (%s): %s", g_prevName, line);
    HANDLE h = CreateFileW(g_prevPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[1100];
    int n = snprintf(buf, sizeof buf, "[next launch, %ls] %s\r\n", g_session, line);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    write_all(h, buf, n);
    CloseHandle(h);
}

// The value of <Data Name='name'> in an event's XML, as UTF-8 ("" if absent).
static void event_data(const WCHAR *xml, const WCHAR *name, char *out, int cap)
{
    WCHAR key[64];
    out[0] = 0;
    _snwprintf(key, 64, L"Name='%s'>", name);
    key[63] = 0;
    const WCHAR *p = wcsstr(xml, key);
    if (!p) return;
    p += wcslen(key);
    const WCHAR *e = wcschr(p, L'<');
    if (!e) return;
    int w = WideCharToMultiByte(CP_UTF8, 0, p, (int)(e - p), out, cap - 1, NULL, NULL);
    out[w > 0 ? w : 0] = 0;
}

// 1: found and reported, 0: not (yet) in the event log, -1: the event log can't be read.
static int look_up(void)
{
    static HMODULE wevt;
    static EvtQuery_t q;
    static EvtNext_t next;
    static EvtRender_t render;
    static EvtClose_t close;
    if (!wevt) {
        wevt = LoadLibraryW(L"wevtapi.dll");
        if (!wevt) return -1;
        q = (EvtQuery_t)GetProcAddress(wevt, "EvtQuery");
        next = (EvtNext_t)GetProcAddress(wevt, "EvtNext");
        render = (EvtRender_t)GetProcAddress(wevt, "EvtRender");
        close = (EvtClose_t)GetProcAddress(wevt, "EvtClose");
    }
    if (!q || !next || !render || !close) return -1;
    WCHAR query[512];
    _snwprintf(query, 512,
               L"*[System[((Provider[@Name='Application Error'] and EventID=1000) or "
               L"(Provider[@Name='Application Hang'] and EventID=1002)) and TimeCreated[@SystemTime>='%s']]]",
               g_since);
    query[511] = 0;
    HANDLE rs = q(NULL, L"Application", query, EVT_QUERY_CHANNEL_PATH | EVT_QUERY_REVERSE);
    if (!rs) return -1;
    int found = 0;
    HANDLE ev[16];
    DWORD got;
    static WCHAR xml[16384];
    while (!found && next(rs, 16, ev, 1000, 0, &got)) {
        for (DWORD i = 0; i < got; i++) {
            DWORD used, props;
            if (!found && render(NULL, ev[i], EVT_RENDER_XML, sizeof xml - sizeof(WCHAR), xml, &used, &props)) {
                xml[used / sizeof(WCHAR) < 16384 ? used / sizeof(WCHAR) : 16383] = 0;
                char pid[32], app[MAX_PATH];
                event_data(xml, L"ProcessId", pid, sizeof pid);
                event_data(xml, L"AppName", app, sizeof app);
                if (strtoul(pid, NULL, 16) == g_prevPid && !_strnicmp(app, "CivilizationV", 13)) {
                    found = 1;
                    if (wcsstr(xml, L"'Application Hang'")) {
                        char type[128];
                        event_data(xml, L"HangType", type, sizeof type);
                        report("Windows recorded a hang (Application Hang): the game stopped responding (%s) and was closed",
                               type[0] ? type : "no hang type");
                    } else {
                        char code[32], mod[MAX_PATH], off[32], ver[64], ts[32];
                        event_data(xml, L"ExceptionCode", code, sizeof code);
                        event_data(xml, L"ModuleName", mod, sizeof mod);
                        event_data(xml, L"FaultingOffset", off, sizeof off);
                        event_data(xml, L"ModuleVersion", ver, sizeof ver);
                        event_data(xml, L"ModuleTimeStamp", ts, sizeof ts);
                        report("Windows recorded the crash (Application Error): exception 0x%s in %s+0x%lx "
                               "(module version %s, timestamp 0x%s)",
                               code, mod, strtoul(off, NULL, 16), ver, ts);
                    }
                }
            }
            close(ev[i]);
        }
    }
    close(rs);
    return found;
}

static DWORD WINAPI look_up_thread(LPVOID unused)
{
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    int r = 0;
    for (int i = 0; i < 7 && !(r = look_up()); i++) Sleep(10000);
    if (r < 0) report("the Windows event log can't be read, so what ended it is unknown");
    else if (!r)
        report("Windows has no Application Error or Application Hang event for pid %lu: it was ended without "
               "one (Task Manager, the power, or error reporting turned off)", g_prevPid);
    return 0;
}

void lastrun_init(const WCHAR *dir, const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"LastRunCheck", 1, ini)) return;
    // The previous session: the newest other events log by creation time (UTC; the local-time
    // stamps in the names don't sort right when the clocks go back).
    WCHAR pat[MAX_PATH], self[40], best[40] = L"";
    ULONGLONG bestCreated = 0;
    _snwprintf(pat, MAX_PATH, L"%s\\events_*.log", dir);
    pat[MAX_PATH - 1] = 0;
    _snwprintf(self, 40, L"events_%s.log", g_session);
    self[39] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        ULONGLONG c = (ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32 | fd.ftCreationTime.dwLowDateTime;
        if (wcslen(fd.cFileName) == 26 && wcscmp(fd.cFileName, self) &&
            (c > bestCreated || (c == bestCreated && wcscmp(fd.cFileName, best) > 0))) {
            lstrcpyW(best, fd.cFileName);
            bestCreated = c;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (!best[0]) return;
    _snwprintf(g_prevPath, MAX_PATH, L"%s\\%s", dir, best);
    g_prevPath[MAX_PATH - 1] = 0;
    WideCharToMultiByte(CP_UTF8, 0, best, -1, g_prevName, sizeof g_prevName, NULL, NULL);

    // Not sharing write: if another instance of the game is still running, it has its log open
    // for writing, so the open fails and that launch isn't reported as ended.
    HANDLE h = CreateFileW(g_prevPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    static char head[1024], tail[4096];
    DWORD n = 0;
    ReadFile(h, head, sizeof head - 1, &n, NULL);
    head[n] = 0;
    LARGE_INTEGER size;
    GetFileSizeEx(h, &size);
    LARGE_INTEGER at = {.QuadPart = size.QuadPart > (LONGLONG)sizeof tail - 1 ? size.QuadPart - (LONGLONG)(sizeof tail - 1) : 0};
    SetFilePointerEx(h, at, NULL, FILE_BEGIN);
    n = 0;
    ReadFile(h, tail, sizeof tail - 1, &n, NULL);
    tail[n] = 0;
    FILETIME created;
    GetFileTime(h, &created, NULL, NULL);
    CloseHandle(h);
    // Ended normally, or already looked up by an earlier launch.
    if (strstr(tail, "] process exiting\r\n") || strstr(tail, "[next launch, ")) return;
    const char *pid = strstr(head, "(pid ");
    if (!pid || head + 300 < pid) return;  // first line only
    g_prevPid = strtoul(pid + 5, NULL, 10);
    SYSTEMTIME st;
    FileTimeToSystemTime(&created, &st);
    _snwprintf(g_since, 40, L"%04d-%02d-%02dT%02d:%02d:%02d.000Z", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_since[39] = 0;
    log_event("previous launch (%s, pid %lu) did not end normally; looking up what Windows recorded", g_prevName, g_prevPid);
    HANDLE th = CreateThread(NULL, 64 * 1024, look_up_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (th) CloseHandle(th);
}
