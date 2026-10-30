// mimalloc redirect test: run as CivilizationV_mitest.exe next to the probe's
// msimg32.dll with MiMalloc=1 (and LuaGcStepMB=64 to check the luagc chaining).
// The manifest binds msvcr90 like the game, so it is loaded when the probe starts;
// mitest_dll.dll and msvcp90.dll are loaded afterwards.
#include <windows.h>
#include <stdio.h>

extern void *(__cdecl *const imp_sbh)(size_t) __asm__("__imp___set_sbh_threshold");  // forces the msvcr90 import
extern void (*const imp_xlen)(void) __asm__("__imp_?_Xlen@_String_base@std@@SAXXZ");  // and msvcp90, like the game

typedef struct {
    int nFailed;
    char failed[24][64];
    int earlyIsMi, mallocIsMi, newIsMi, strdupIsMi, staleReadable, largeIsCrt;
    DWORD freeBeforeMb, freeFullMb, freeAfterMb;
} MiTestResult;

int main(void)
{
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    printf("msvcr90 import at %p, msvcp90 import at %p\n", (void *)imp_sbh, (void *)imp_xlen);
    HMODULE cpp = LoadLibraryA("msvcp90.dll");
    HMODULE dll = LoadLibraryA("mitest_dll.dll");
    if (!dll) { printf("cannot load mitest_dll.dll (%lu)\n", GetLastError()); return 1; }
    MiTestResult *(__cdecl *run)(void) = (void *)GetProcAddress(dll, "mitest_run");
    MiTestResult *r = run();
    printf("msvcp90 %s\n", cpp ? "loaded" : "NOT loaded");
    printf("in mimalloc: early block %d, malloc %d, new %d; CRT _strdup block %d (expect 1 1 1 0)\n",
           r->earlyIsMi, r->mallocIsMi, r->newIsMi, r->strdupIsMi);
    printf("free address space: %lu MB before, %lu MB with 200 MB of small blocks, %lu MB after freeing\n",
           r->freeBeforeMb, r->freeFullMb, r->freeAfterMb);
    printf("freed blocks still readable (bits 2K/8K/16K): %d (expect 7); large blocks on the CRT heap: %d (expect 1)\n", r->staleReadable, r->largeIsCrt);
    for (int i = 0; i < r->nFailed && i < 24; i++) printf("FAILED: %s\n", r->failed[i]);
    int ok = !r->nFailed && r->earlyIsMi && r->mallocIsMi && r->newIsMi && !r->strdupIsMi && r->staleReadable == 7 && r->largeIsCrt;
    printf("%s\n", ok ? "ALL PASSED" : "FAILURES");
    Sleep(2500);  // let the sampler write a report
    return ok ? 0 : 1;
}