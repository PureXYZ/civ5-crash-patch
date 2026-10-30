// Stack scan test. The parent runs itself as a child three times; each child crashes after
// writing the frames it expects in the events log to stacktest_<mode>.txt: "exe" in a call
// chain inside this exe, "dll" inside stacktest_dll.dll (loaded at run time), "overflow" by
// unbounded recursion. The parent then checks the events log of that child: the "stack:" lines
// hold the expected frames in order, and for the overflow the recursion is one frame with a
// count, followed later by the call from main. Other frames may come between: the scan also
// finds stale return addresses left in locals.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ULONG_PTR g_base;
static const char *g_name;  // this exe's file name
static const char *g_mode;
static void *g_ret[3];      // return addresses into level2, level1 and main

static int g_ok = 1;
static void check(int good, const char *what) { printf("%-58s %s\n", what, good ? "ok" : "WRONG"); if (!good) g_ok = 0; }

static int frame(char *out, int cap, const char *mod, ULONG_PTR base, void *ret)
{
    return snprintf(out, cap, " %s+0x%lx", mod, (unsigned long)((ULONG_PTR)ret - base));
}

static void write_expect(const char *text)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    snprintf(strrchr(path, '\\') + 1, 64, "stacktest_%s.txt", g_mode);
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

// The chain below level3 is the same for both crashes; inner frames come first.
static void expect_chain(char *buf, int cap, int n)
{
    for (int i = 0; i < 3; i++) n += frame(buf + n, cap - n, g_name, g_base, g_ret[i]);
    write_expect(buf);
}

typedef void (*before_crash_t)(void *, void *);
typedef int (*dll_entry_t)(volatile int *, before_crash_t);

static void before_dll_crash(void *retIntoEntry, void *retIntoLevel3)
{
    char buf[512];
    int n = frame(buf, sizeof buf, "stacktest_dll.dll", (ULONG_PTR)GetModuleHandleA("stacktest_dll.dll"), retIntoEntry);
    n += frame(buf + n, sizeof buf - n, g_name, g_base, retIntoLevel3);
    expect_chain(buf, sizeof buf, n);
}

__attribute__((noinline)) int level3(volatile int *p, int dll)
{
    g_ret[0] = __builtin_return_address(0);
    if (dll) {
        HMODULE h = LoadLibraryA("stacktest_dll.dll");
        dll_entry_t f = h ? (dll_entry_t)GetProcAddress(h, "stacktest_dll_entry") : NULL;
        return f ? f(p, before_dll_crash) + 1 : -1;
    }
    char buf[512];
    expect_chain(buf, sizeof buf, 0);
    return *p + 1;
}
__attribute__((noinline)) int level2(volatile int *p, int dll) { g_ret[1] = __builtin_return_address(0); return level3(p, dll) * 3; }
__attribute__((noinline)) int level1(volatile int *p, int dll) { g_ret[2] = __builtin_return_address(0); return level2(p, dll) + 5; }

// At depth 1 both frames are known: the recursion's return address and main's call.
static void *g_fromMain;
static volatile int g_never = -1;
__attribute__((noinline)) int recurse(int d)
{
    volatile char buf[256];
    if (d == g_never) return 0;
    if (d == 0) g_fromMain = __builtin_return_address(0);
    if (d == 1) {
        char text[256];
        int n = frame(text, sizeof text, g_name, g_base, __builtin_return_address(0));
        frame(text + n, sizeof text - n, g_name, g_base, g_fromMain);
        write_expect(text);
    }
    buf[0] = (char)d;
    return recurse(d + 1) + buf[0];
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep) { ExitProcess(3); }

static int child(const char *mode)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    SetUnhandledExceptionFilter(unhandled);
    static char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    g_name = strrchr(exe, '\\') + 1;
    g_base = (ULONG_PTR)GetModuleHandleA(NULL);
    g_mode = mode;
    if (!strcmp(mode, "overflow")) return recurse(0);
    return level1((volatile int *)0x10, !strcmp(mode, "dll"));
}

// The newest events log (names sort by time) read into buf.
static int read_newest_log(const char *dir, char *buf, int cap)
{
    char pat[MAX_PATH], best[MAX_PATH] = "";
    snprintf(pat, sizeof pat, "%s\\civprobe\\events_*.log", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do if (strcmp(fd.cFileName, best) > 0) strcpy(best, fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h);
    snprintf(pat, sizeof pat, "%s\\civprobe\\%s", dir, best);
    FILE *f = fopen(pat, "rb");
    if (!f) return 0;
    int n = (int)fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

// Runs the child for mode; its expected frames go to expect and its events log to log.
static int run_child(const char *exe, const char *dir, const char *mode, char *expect, int cap, char *log, int logCap)
{
    Sleep(1100);  // a later second than the previous launch: its events log sorts last
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, mode);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0;
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\stacktest_%s.txt", dir, mode);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = (int)fread(expect, 1, cap - 1, f);
    fclose(f);
    expect[n] = 0;
    DeleteFileA(path);
    return read_newest_log(dir, log, logCap);
}

// The frames of every "stack:" line, joined, each with a space before and after.
static const char *stack_frames(const char *log, char *out, int cap)
{
    int n = 0;
    for (const char *p = log; (p = strstr(p, "] stack: ")); ) {
        p += 8;
        while (*p && *p != '\r' && *p != '\n' && n < cap - 2) out[n++] = *p++;
    }
    out[n++] = ' ';
    out[n] = 0;
    return out;
}

// The frames in want (each after a space) occur in got in the same order.
static int in_order(const char *want, const char *got)
{
    char frame[128], key[132];
    for (const char *w = want; sscanf(w, " %127s", frame) == 1; w = strstr(w, frame) + strlen(frame)) {
        snprintf(key, sizeof key, " %s ", frame);
        if (!(got = strstr(got, key))) return 0;
        got += strlen(key) - 1;
    }
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    if (argc > 1) return child(argv[1]);

    char exe[MAX_PATH], dir[MAX_PATH], expect[1024], want[1100];
    static char log[1 << 20], got[8192];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(dir, exe);
    *strrchr(dir, '\\') = 0;

    check(run_child(exe, dir, "exe", expect, sizeof expect, log, sizeof log), "exe: child ran, log read");
    printf("  want%s\n  got %s\n", expect, stack_frames(log, got, sizeof got));
    check(in_order(expect, got), "exe: level3's callers up to main, in order");

    check(run_child(exe, dir, "dll", expect, sizeof expect, log, sizeof log), "dll: child ran, log read");
    printf("  want%s\n  got %s\n", expect, stack_frames(log, got, sizeof got));
    check(strstr(log, "module loaded: ") && strstr(log, "\\stacktest_dll.dll\r\n"), "dll: the run-time load is logged");
    check(strstr(log, "(stacktest_dll.dll+0x") != NULL, "dll: exception named in the DLL");
    check(in_order(expect, got), "dll: from the DLL through level3 to main, in order");

    check(run_child(exe, dir, "overflow", expect, sizeof expect, log, sizeof log), "overflow: child ran, log read");
    printf("  want%s\n  got %s\n", expect, stack_frames(log, got, sizeof got));
    check(strstr(log, "exception 0xc00000fd") != NULL, "overflow: exception logged");
    char *second = strchr(expect + 1, ' ');
    snprintf(want, sizeof want, "%.*s (x", second ? (int)(second - expect) : 0, expect);
    check(second && !strncmp(got, want, strlen(want)), "overflow: the recursion first, as one frame with a count");
    check(in_order(expect, got), "overflow: then the call from main");

    printf("%s\n", g_ok ? "ALL PASSED" : "FAILURES");
    return g_ok ? 0 : 1;
}
