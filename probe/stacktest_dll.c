// Stack scan test DLL: loaded at run time by CivilizationV_stacktest.exe, so its frames are
// only named if the probe's module table picked up the load.
#include <windows.h>

typedef void (*before_crash_t)(void *retIntoEntry, void *retIntoCaller);

__attribute__((noinline)) static int crash_here(volatile int *p, before_crash_t before, void *retIntoCaller)
{
    before(__builtin_return_address(0), retIntoCaller);
    return *p + 1;
}

__declspec(dllexport) __attribute__((noinline)) int stacktest_dll_entry(volatile int *p, before_crash_t before)
{
    return crash_here(p, before, __builtin_return_address(0)) * 2;
}
