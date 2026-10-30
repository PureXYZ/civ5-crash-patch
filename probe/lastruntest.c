// Previous-launch check test (lastrun.c). The parent runs itself as a child three times:
//   crash  an unhandled access violation with no handler of its own: Windows ends it and
//          records "Application Error" (1000) for its pid; its log has no "process exiting"
//   next   a normal launch: waits for the probe's lookup, then exits normally
//   again  a normal launch after a normal one: nothing to look up
// The parent checks that "next" appended Windows' record to the crash child's log (when
// Windows has one for that pid: error reporting can be off, e.g. on build servers, and then
// the "no event" line is expected), and that "again" left the clean log alone.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_ok = 1;
static void check(int good, const char *what) { printf("%-58s %s\n", what, good ? "ok" : "WRONG"); if (!good) g_ok = 0; }

// The newest events log (names sort by time): its name into name, its text into buf.
static int read_newest_log(const char *dir, char *name, char *buf, int cap)
{
    char pat[MAX_PATH], best[MAX_PATH] = "";
    snprintf(pat, sizeof pat, "%s\\civprobe\\events_*.log", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do if (strcmp(fd.cFileName, best) > 0) strcpy(best, fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h);
    if (name) strcpy(name, best);
    snprintf(pat, sizeof pat, "%s\\civprobe\\%s", dir, best);
    FILE *f = fopen(pat, "rb");
    if (!f) return 0;
    int n = (int)fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

static int read_log(const char *dir, const char *name, char *buf, int cap)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\civprobe\\%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = (int)fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

static int child(const char *mode, const char *dir)
{
    if (!strcmp(mode, "crash")) {
        HRESULT(WINAPI *wer_flags)(DWORD) = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "WerSetFlags");
        if (wer_flags) wer_flags(4 /* WER_FAULT_REPORTING_NO_UI */);
        *(volatile int *)0x10 = 1;
        return 2;
    }
    if (!strcmp(mode, "next")) {
        // The lookup retries for about a minute; it ends with a second "previous launch" line.
        static char log[1 << 16];
        for (int i = 0; i < 100; i++) {
            Sleep(1000);
            read_newest_log(dir, NULL, log, sizeof log);
            const char *p = strstr(log, "] previous launch (");
            if (p && strstr(p + 1, "] previous launch (")) break;
        }
    }
    return 0;
}

static DWORD run_child(const char *exe, const char *mode)
{
    Sleep(1100);  // a later second than the previous launch: its events log sorts last
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, mode);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0;
    if (WaitForSingleObject(pi.hProcess, 150000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return strcmp(mode, "crash") ? code : pid;
}

// Whether Windows recorded Application Error for pid (the parent's own lookup).
static int windows_has_crash(DWORD pid)
{
    typedef HANDLE(WINAPI * q_t)(HANDLE, LPCWSTR, LPCWSTR, DWORD);
    typedef BOOL(WINAPI * n_t)(HANDLE, DWORD, HANDLE *, DWORD, DWORD, PDWORD);
    typedef BOOL(WINAPI * r_t)(HANDLE, HANDLE, DWORD, DWORD, PVOID, PDWORD, PDWORD);
    typedef BOOL(WINAPI * c_t)(HANDLE);
    HMODULE w = LoadLibraryA("wevtapi.dll");
    q_t q = w ? (q_t)GetProcAddress(w, "EvtQuery") : NULL;
    n_t next = w ? (n_t)GetProcAddress(w, "EvtNext") : NULL;
    r_t render = w ? (r_t)GetProcAddress(w, "EvtRender") : NULL;
    c_t close = w ? (c_t)GetProcAddress(w, "EvtClose") : NULL;
    if (!q || !next || !render || !close) return 0;
    HANDLE rs = q(NULL, L"Application", L"*[System[Provider[@Name='Application Error'] and EventID=1000]]", 0x1 | 0x200);
    if (!rs) return 0;
    WCHAR key[64];
    _snwprintf(key, 64, L"<Data Name='ProcessId'>0x%lx</Data>", pid);
    static WCHAR xml[16384];
    HANDLE ev[16];
    DWORD got, used, props;
    int found = 0;
    for (int batch = 0; !found && batch < 8 && next(rs, 16, ev, 1000, 0, &got); batch++)
        for (DWORD i = 0; i < got; i++) {
            if (!found && render(NULL, ev[i], 1, sizeof xml - 2, xml, &used, &props)) {
                xml[used / 2 < 16384 ? used / 2 : 16383] = 0;
                found = wcsstr(xml, key) && wcsstr(xml, L"CivilizationV_lastruntest.exe");
            }
            close(ev[i]);
        }
    close(rs);
    return found;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    char exe[MAX_PATH], dir[MAX_PATH], crashLog[64], nextLog[64];
    static char log[1 << 20];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(dir, exe);
    *strrchr(dir, '\\') = 0;
    if (argc > 1) return child(argv[1], dir);

    DWORD pid = run_child(exe, "crash");
    check(pid && read_newest_log(dir, crashLog, log, sizeof log), "crash: child ran, log read");
    check(!strstr(log, "] process exiting\r\n"), "crash: its log has no \"process exiting\"");

    printf("  waiting for the next launch's lookup (up to about 70 s)\n");
    check(run_child(exe, "next") == 0, "next: child ran");
    read_newest_log(dir, nextLog, log, sizeof log);
    char want[128];
    snprintf(want, sizeof want, "previous launch (%s, pid %lu) did not end normally", crashLog, pid);
    check(strstr(log, want) != NULL, "next: noticed the previous launch didn't end normally");

    read_log(dir, crashLog, log, sizeof log);
    const char *line = strstr(log, "[next launch, ");
    printf("  %.*s\n", line ? (int)(strcspn(line, "\r\n")) : 4, line ? line : "none");
    if (windows_has_crash(pid)) {
        check(line && strstr(line, "Windows recorded the crash (Application Error): exception 0xc0000005 in "
                                   "CivilizationV_lastruntest.exe+0x"), "crash log: Windows' record appended");
    } else {
        printf("  Windows has no Application Error event for pid %lu (error reporting off?)\n", pid);
        check(line && strstr(line, "Windows has no Application Error or Application Hang event"), "crash log: \"no event\" appended");
    }

    check(run_child(exe, "again") == 0, "again: child ran");
    read_newest_log(dir, NULL, log, sizeof log);
    check(!strstr(log, "previous launch ("), "again: a normal previous launch is left alone");
    read_log(dir, nextLog, log, sizeof log);
    check(!strstr(log, "[next launch, "), "again: nothing appended to the normal log");

    printf("%s\n", g_ok ? "ALL PASSED" : "FAILURES");
    return g_ok ? 0 : 1;
}
