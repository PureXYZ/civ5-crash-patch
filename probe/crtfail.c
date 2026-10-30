// Logs the C runtime's own ways of ending the game ([probe] CrtFailureLog=1, the default).
// None of them reach the exception handler in probe.c:
//   - the stack-cookie check (/GS, 0xC0000409) and invalid-parameter reports (0xC0000417):
//     the VS2008 runtime the game uses reports both by calling UnhandledExceptionFilter
//     directly. The /GS check is linked into each module (the exe and the game DLLs import
//     UnhandledExceptionFilter for it); Watson is in msvcr90.dll.
//   - abort(), terminate() and pure virtual calls (_purecall, R6025). abort() also reports
//     through UnhandledExceptionFilter after writing its message.
//   - uncaught C++ exceptions, which end in terminate() -> abort() inside msvcr90. The
//     exception handler notes the type of the last C++ exception thrown (crtfail_note_cxx),
//     and the lines below name it when it was this thread's.
// The imports of UnhandledExceptionFilter, abort, terminate and _purecall in the exe,
// msvcr90/msvcp90 and every DLL from outside the Windows folder (the VP DLL loads later) are
// pointed at wrappers that log the call, its caller and the stack, then call the original.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "probe.h"

BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE *, DWORD, LPDWORD);

typedef LONG(WINAPI *uef_t)(EXCEPTION_POINTERS *);
typedef void(__cdecl *crt_void_t)(void);
static uef_t real_uef;
static crt_void_t real_abort, real_terminate, real_purecall;
static HMODULE g_self, g_exe;
static WCHAR g_winDir[MAX_PATH];
static int g_winDirLen;
static volatile LONG g_reports;
#define MAX_REPORTS 8

// The last C++ exception thrown (any thread) and its type, e.g. ".?AVbad_alloc@std@@".
static volatile DWORD g_cxxTid;
static char g_cxxType[96];

void crtfail_note_cxx(const EXCEPTION_RECORD *er)
{
    // MSVC x86: parameters are magic 0x19930520, the object, the ThrowInfo. ThrowInfo+12 ->
    // CatchableTypeArray {count, types[]}; CatchableType+4 -> TypeDescriptor, its name at +8.
    // Each pointer must lie in a loaded module before it is read.
    ULONG_PTR b;
    if (!real_uef) return;  // off
    if (er->NumberParameters < 3 || er->ExceptionInformation[0] != 0x19930520) return;
    const BYTE *ti = (const BYTE *)er->ExceptionInformation[2];
    if (!ti || !module_at(ti, &b)) return;
    const BYTE *cta = *(const BYTE *const *)(ti + 12);
    if (!module_at(cta, &b) || *(const int *)cta < 1) return;
    const BYTE *ct = *(const BYTE *const *)(cta + 4);
    if (!module_at(ct, &b)) return;
    const BYTE *td = *(const BYTE *const *)(ct + 4);
    if (!module_at(td, &b)) return;
    const char *name = (const char *)(td + 8);
    int n = 0;
    while (n < (int)sizeof g_cxxType - 1 && name[n] && module_at(name + n, &b)) { g_cxxType[n] = name[n]; n++; }
    g_cxxType[n] = 0;
    g_cxxTid = GetCurrentThreadId();
}

// At exit: the last C++ exception thrown, if any (an abort from inside the CRT, as after an
// uncaught exception, never reaches the hooked abort import, so report() doesn't name it).
void crtfail_log_last_cxx(void)
{
    if (g_cxxTid) log_event("crt: last C++ exception: %s, thread %lu", g_cxxType, g_cxxTid);
}

static const char *status_name(DWORD code)
{
    switch (code) {
    case 0xC0000409: return "stack buffer overrun (/GS check)";
    case 0xC0000417: return "invalid parameter to a CRT function";
    case 0x40000015: return "fatal app exit (abort)";
    default: return "exception";
    }
}

static void name_addr(const void *a, char *out, int cap)
{
    ULONG_PTR mb;
    const char *mname = module_at(a, &mb);
    if (mname) snprintf(out, cap, "%s+0x%lx", mname, (unsigned long)((ULONG_PTR)a - mb));
    else snprintf(out, cap, "%p", a);
}

// Logs what (called from ret) and the stack from esp, or from the caller's frame when esp is
// not on this thread's stack.
static void report(const char *what, void *ret, ULONG_PTR esp, ULONG_PTR ownEsp)
{
    if (InterlockedIncrement(&g_reports) > MAX_REPORTS) return;
    char from[96];
    name_addr(ret, from, sizeof from);
    log_event("crt: %s called from %s, thread %lu", what, from, GetCurrentThreadId());
    if (g_cxxTid == GetCurrentThreadId()) log_event("crt: last C++ exception on this thread: %s", g_cxxType);
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    CONTEXT ctx;
    ctx.Esp = esp >= (ULONG_PTR)tib->StackLimit && esp < (ULONG_PTR)tib->StackBase ? esp : ownEsp;
    modules_log_stack(&ctx, tib, 0);
    log_memory_now("at CRT failure");
}

// The slot holding the wrapper's return address: the caller's frame starts there.
#define OWN_ESP ((ULONG_PTR)__builtin_frame_address(0) + 4)

static LONG WINAPI hook_uef(EXCEPTION_POINTERS *ep)
{
    // For /GS the exception address is the function whose stack cookie was overwritten.
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    char at[96] = "?", what[192];
    if (ep && ep->ExceptionRecord) name_addr(ep->ExceptionRecord->ExceptionAddress, at, sizeof at);
    snprintf(what, sizeof what, "UnhandledExceptionFilter(0x%08lx: %s at %s)", code, status_name(code), at);
    report(what, __builtin_return_address(0), ep && ep->ContextRecord ? ep->ContextRecord->Esp : 0, OWN_ESP);
    return real_uef(ep);
}

static void __cdecl hook_abort(void)
{
    report("abort()", __builtin_return_address(0), 0, OWN_ESP);
    real_abort();
}

static void __cdecl hook_terminate(void)
{
    report("terminate()", __builtin_return_address(0), 0, OWN_ESP);
    real_terminate();
}

static void __cdecl hook_purecall(void)
{
    report("pure virtual call (_purecall)", __builtin_return_address(0), 0, OWN_ESP);
    real_purecall();
}

// Ordinary exits, so a launch that quits by itself shows which code decided to: PostQuitMessage,
// exit/_exit and ExitProcess, and TerminateProcess on this process. Normal quits log the same way.
typedef void(WINAPI *pqm_t)(int);
typedef void(__cdecl *crt_exit_t)(int);
typedef void(WINAPI *exitproc_t)(UINT);
typedef BOOL(WINAPI *termproc_t)(HANDLE, UINT);
static pqm_t real_pqm;
static crt_exit_t real_exit, real__exit;
static exitproc_t real_exitproc;
static termproc_t real_termproc;
static volatile LONG g_exitReports;
#define MAX_EXIT_REPORTS 6

static void exit_report(const char *what, int code, void *ret)
{
    if (InterlockedIncrement(&g_exitReports) > MAX_EXIT_REPORTS) return;
    char from[96];
    name_addr(ret, from, sizeof from);
    log_event("exit: %s(%d) called from %s, thread %lu", what, code, from, GetCurrentThreadId());
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    CONTEXT ctx;
    ctx.Esp = (ULONG_PTR)__builtin_frame_address(0);
    modules_log_stack(&ctx, tib, 0);
}

static void WINAPI hook_pqm(int code) { exit_report("PostQuitMessage", code, __builtin_return_address(0)); real_pqm(code); }
static void __cdecl hook_exit(int code) { exit_report("exit", code, __builtin_return_address(0)); real_exit(code); }
static void __cdecl hook__exit(int code) { exit_report("_exit", code, __builtin_return_address(0)); real__exit(code); }
static void WINAPI hook_exitproc(UINT code) { exit_report("ExitProcess", (int)code, __builtin_return_address(0)); real_exitproc(code); }
static BOOL WINAPI hook_termproc(HANDLE h, UINT code)
{
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId())
        exit_report("TerminateProcess", (int)code, __builtin_return_address(0));
    return real_termproc(h, code);
}

static int wanted(BYTE *base)
{
    if ((HMODULE)base == g_self) return 0;
    if ((HMODULE)base == g_exe) return 1;
    WCHAR path[MAX_PATH];
    int len = (int)GetModuleFileNameW((HMODULE)base, path, MAX_PATH);
    if (!len) return 0;
    const WCHAR *file = wcsrchr(path, L'\\');
    file = file ? file + 1 : path;
    if (!_wcsnicmp(file, L"msvcr90", 7) || !_wcsnicmp(file, L"msvcp90", 7)) return 1;
    return !(g_winDirLen && len >= g_winDirLen && !_wcsnicmp(path, g_winDir, g_winDirLen));
}

static void patch_module(BYTE *base)
{
    if (!wanted(base)) return;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); dir.Size && d->Name; d++)
        for (IMAGE_THUNK_DATA32 *t = (IMAGE_THUNK_DATA32 *)(base + d->FirstThunk); t->u1.Function; t++) {
            DWORD f = t->u1.Function;
            void *to = f == (DWORD)(ULONG_PTR)real_uef                       ? (void *)hook_uef
                       : real_abort && f == (DWORD)(ULONG_PTR)real_abort         ? (void *)hook_abort
                       : real_terminate && f == (DWORD)(ULONG_PTR)real_terminate ? (void *)hook_terminate
                       : real_purecall && f == (DWORD)(ULONG_PTR)real_purecall   ? (void *)hook_purecall
                       : real_pqm && f == (DWORD)(ULONG_PTR)real_pqm             ? (void *)hook_pqm
                       : real_exit && f == (DWORD)(ULONG_PTR)real_exit           ? (void *)hook_exit
                       : real__exit && f == (DWORD)(ULONG_PTR)real__exit         ? (void *)hook__exit
                       : real_exitproc && f == (DWORD)(ULONG_PTR)real_exitproc   ? (void *)hook_exitproc
                       : real_termproc && f == (DWORD)(ULONG_PTR)real_termproc   ? (void *)hook_termproc
                                                                                 : NULL;
            if (!to) continue;
            // Some linkers put the import table in a code page: keep it executable meanwhile.
            MEMORY_BASIC_INFORMATION mbi;
            DWORD rw = VirtualQuery(&t->u1.Function, &mbi, sizeof mbi) &&
                               (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
                           ? PAGE_EXECUTE_READWRITE
                           : PAGE_READWRITE;
            DWORD old;
            if (!VirtualProtect(&t->u1.Function, 4, rw, &old)) continue;
            InterlockedExchange((volatile LONG *)&t->u1.Function, (LONG)(ULONG_PTR)to);
            VirtualProtect(&t->u1.Function, 4, old, &old);
            n++;
        }
    if (n) {
        WCHAR name[MAX_PATH];
        if (!GetModuleFileNameW((HMODULE)base, name, MAX_PATH)) name[0] = 0;
        const WCHAR *s = wcsrchr(name, L'\\');
        log_event("crt: watching %d failure import(s) of %ls", n, s ? s + 1 : name);
    }
}

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } CfUstr;
typedef struct { ULONG Flags; const CfUstr *FullDllName; const CfUstr *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } CfNotifyData;
typedef VOID(CALLBACK *CfNotify_t)(ULONG reason, const CfNotifyData *data, PVOID ctx);
typedef LONG(NTAPI *CfRegister_t)(ULONG, CfNotify_t, PVOID, PVOID *);

static VOID CALLBACK on_dll_notify(ULONG reason, const CfNotifyData *data, PVOID ctx)
{
    (void)ctx;
    if (reason == 1 /* loaded */) patch_module((BYTE *)data->DllBase);
}

void crtfail_init(const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"CrtFailureLog", 1, ini)) return;
    int len = (int)GetWindowsDirectoryW(g_winDir, MAX_PATH - 1);
    if (len > 0 && len < MAX_PATH - 1) {
        if (g_winDir[len - 1] != L'\\') g_winDir[len++] = L'\\';
        g_winDir[len] = 0;
        g_winDirLen = len;
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)crtfail_init, &g_self);
    g_exe = GetModuleHandleW(NULL);
    real_uef = (uef_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "UnhandledExceptionFilter");
    HMODULE crt = GetModuleHandleW(L"msvcr90.dll");
    if (crt) {
        real_abort = (crt_void_t)GetProcAddress(crt, "abort");
        real_terminate = (crt_void_t)GetProcAddress(crt, "?terminate@@YAXXZ");
        real_purecall = (crt_void_t)GetProcAddress(crt, "_purecall");
        real_exit = (crt_exit_t)GetProcAddress(crt, "exit");
        real__exit = (crt_exit_t)GetProcAddress(crt, "_exit");
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll"), u32 = GetModuleHandleW(L"user32.dll");
    real_exitproc = (exitproc_t)GetProcAddress(k32, "ExitProcess");
    real_termproc = (termproc_t)GetProcAddress(k32, "TerminateProcess");
    if (u32) real_pqm = (pqm_t)GetProcAddress(u32, "PostQuitMessage");
    if (!real_uef) { log_event("crt: UnhandledExceptionFilter not found; off"); return; }
    if (!crt) log_event("crt: msvcr90.dll not loaded; only UnhandledExceptionFilter is watched");
    HMODULE mods[512];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        if (count > 512) count = 512;
        for (DWORD i = 0; i < count; i++) patch_module((BYTE *)mods[i]);
    }
    CfRegister_t reg = (CfRegister_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    PVOID cookie;
    if (reg) reg(0, on_dll_notify, NULL, &cookie);
}
