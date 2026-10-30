// CRT failure test (crtfail.c). The parent runs itself as a child once per failure; each child
// loads crttest_dll.dll, which imports the failure paths from msvcr90 like a game module, and
// ends the process through one of them:
//   gs        the /GS check's report: UnhandledExceptionFilter(0xC0000409)
//   invparam  _invalid_parameter_noinfo: msvcr90's Watson report, UnhandledExceptionFilter(0xC0000417)
//   abort     abort()
//   purecall  _purecall (a pure virtual call)
//   cxx       a C++ exception (raised by hand with MSVC's layout, handled), then terminate()
// The parent checks each child's events log for the "crt:" lines, their caller, the stack and
// the memory line. The manifest binds msvcr90 like the game, so it is loaded when the probe starts.
#include <windows.h>
#include <stdio.h>
#include <string.h>

extern void *(__cdecl *const imp_sbh)(size_t) __asm__("__imp___set_sbh_threshold");  // forces the msvcr90 import

static int g_ok = 1;
static void check(int good, const char *what) { printf("%-58s %s\n", what, good ? "ok" : "WRONG"); if (!good) g_ok = 0; }

// MSVC x86 throw metadata for a type "test_error": ThrowInfo -> CatchableTypeArray ->
// CatchableType -> TypeDescriptor (name at +8).
static const struct { void *vft, *spare; char name[20]; } g_td = {0, 0, ".?AVtest_error@@"};
static const struct { unsigned props; const void *type; int pmd[3]; int size; void *copy; } g_ct = {0, &g_td, {0, -1, 0}, 4, 0};
static const struct { int n; const void *types[1]; } g_cta = {1, {&g_ct}};
static const struct { unsigned attrs; void *unwind, *forward; const void *cta; } g_ti = {0, 0, 0, &g_cta};

static LONG CALLBACK continue_cxx(EXCEPTION_POINTERS *ep)
{
    return ep->ExceptionRecord->ExceptionCode == 0xE06D7363 ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS *ep) { ExitProcess(3); }

static int child(const char *mode)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    HRESULT(WINAPI *wer_flags)(DWORD) = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "WerSetFlags");
    if (wer_flags) wer_flags(4 /* WER_FAULT_REPORTING_NO_UI */);
    SetUnhandledExceptionFilter(unhandled);
    // msvcr90 shows abort's and R6025's message in a box unless the app says it is a console
    // app (the game's own CRT startup does this); without it each child waits for a click.
    HMODULE crt = GetModuleHandleA("msvcr90.dll");
    void(__cdecl *set_app_type)(int) = (void *)GetProcAddress(crt, "__set_app_type");
    if (set_app_type) set_app_type(1 /* _CONSOLE_APP */);
    HMODULE dll = LoadLibraryA("crttest_dll.dll");
    if (!dll) { printf("cannot load crttest_dll.dll (%lu)\n", GetLastError()); return 1; }
    char fn[64];
    snprintf(fn, sizeof fn, "crttest_%s", strcmp(mode, "cxx") ? mode : "terminate");
    void (*f)(void) = (void (*)(void))GetProcAddress(dll, fn);
    if (!f) { printf("no %s in crttest_dll.dll\n", fn); return 1; }
    if (!strcmp(mode, "cxx")) {
        AddVectoredExceptionHandler(0, continue_cxx);  // after the probe's handler
        int obj = 0;
        ULONG_PTR params[3] = {0x19930520, (ULONG_PTR)&obj, (ULONG_PTR)&g_ti};
        RaiseException(0xE06D7363, 0, 3, params);
    }
    f();
    return 2;  // not reached
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

static int run_child(const char *exe, const char *dir, const char *mode, char *log, int cap)
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
    return read_newest_log(dir, log, cap);
}

// A "crt:" line starting with what and naming caller, followed by a stack line naming the
// test DLL, and the memory line.
static int crt_report(const char *log, const char *what, const char *caller)
{
    const char *p = strstr(log, what);
    if (!p) return 0;
    const char *c = strstr(p, caller);
    if (!c || c > strchr(p, '\n')) return 0;
    const char *st = strstr(p, "] stack: ");
    const char *eol = st ? strchr(st, '\n') : NULL;
    const char *dll = st ? strstr(st, "crttest_dll.dll+0x") : NULL;
    return dll && dll < eol && strstr(p, "memory (at CRT failure)");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    if (argc > 1) return child(argv[1]);
    printf("msvcr90 import at %p\n", (void *)imp_sbh);

    char exe[MAX_PATH], dir[MAX_PATH];
    static char log[1 << 20];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(dir, exe);
    *strrchr(dir, '\\') = 0;

    check(run_child(exe, dir, "gs", log, sizeof log), "gs: child ran, log read");
    check(strstr(log, "crt: watching ") && strstr(log, "of msvcr90.dll"), "msvcr90's imports watched at start");
    check(strstr(log, "of crttest_dll.dll") != NULL, "the run-time DLL's imports watched when it loads");
    check(crt_report(log, "crt: UnhandledExceptionFilter(0xc0000409: stack buffer overrun (/GS check) at crttest_dll.dll+0x",
                     ") called from crttest_dll.dll+0x"), "gs: logged with the function, caller, stack and memory");

    check(run_child(exe, dir, "invparam", log, sizeof log), "invparam: child ran, log read");
    check(crt_report(log, "crt: UnhandledExceptionFilter(0xc0000417: invalid parameter to a CRT function at ",
                     ") called from msvcr90.dll+0x"), "invparam: Watson's report logged, stack reaches the DLL");

    check(run_child(exe, dir, "abort", log, sizeof log), "abort: child ran, log read");
    check(crt_report(log, "crt: abort()", " called from crttest_dll.dll+0x"), "abort: logged with caller, stack and memory");

    check(run_child(exe, dir, "purecall", log, sizeof log), "purecall: child ran, log read");
    check(crt_report(log, "crt: pure virtual call (_purecall)", " called from crttest_dll.dll+0x"), "purecall: logged with caller, stack and memory");

    check(run_child(exe, dir, "cxx", log, sizeof log), "cxx: child ran, log read");
    check(crt_report(log, "crt: terminate()", " called from crttest_dll.dll+0x"), "cxx: terminate() logged with caller, stack and memory");
    check(strstr(log, "crt: last C++ exception on this thread: .?AVtest_error@@\r\n") != NULL, "cxx: the C++ exception's type is named");

    printf("%s\n", g_ok ? "ALL PASSED" : "FAILURES");
    return g_ok ? 0 : 1;
}
