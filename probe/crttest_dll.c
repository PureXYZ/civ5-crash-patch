// CRT failure test DLL: no CRT of its own, it imports the failure paths from msvcr90 like the
// game's modules (UnhandledExceptionFilter from kernel32, as the /GS check linked into each
// module does). Loaded at run time by CivilizationV_crttest.exe, so its imports are only
// watched if crtfail.c patches DLLs as they load.
#include <windows.h>

extern void(__cdecl *const imp_abort)(void) __asm__("__imp__abort");
extern void(__cdecl *const imp_purecall)(void) __asm__("__imp___purecall");
extern void(__cdecl *const imp_invparam)(void) __asm__("__imp___invalid_parameter_noinfo");
extern void(__cdecl *const imp_terminate)(void) __asm__("__imp_?terminate@@YAXXZ");

__declspec(dllexport) void crttest_abort(void) { imp_abort(); }
__declspec(dllexport) void crttest_purecall(void) { imp_purecall(); }
__declspec(dllexport) void crttest_invparam(void) { imp_invparam(); }
__declspec(dllexport) void crttest_terminate(void) { imp_terminate(); }

// What VS2008's __report_gsfailure does on a bad cookie: a context for the function whose
// cookie failed (Eip = the return address into it, Esp = its frame), reported through
// UnhandledExceptionFilter, then the process ends.
__attribute__((noinline)) static void report_gsfailure(void)
{
    static CONTEXT ctx;
    static EXCEPTION_RECORD er;
    static EXCEPTION_POINTERS ep = {&er, &ctx};
    ctx.Eip = (DWORD)(ULONG_PTR)__builtin_return_address(0);
    ctx.Esp = (DWORD)(ULONG_PTR)__builtin_frame_address(0) + 8;
    er.ExceptionCode = 0xC0000409;
    er.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    er.ExceptionAddress = (void *)(ULONG_PTR)ctx.Eip;
    UnhandledExceptionFilter(&ep);
    TerminateProcess(GetCurrentProcess(), 0xC0000409);
}

__attribute__((noinline)) static void gs_victim(void) { report_gsfailure(); }
__declspec(dllexport) void crttest_gs(void) { gs_victim(); }

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD reason, LPVOID r) { return TRUE; }
