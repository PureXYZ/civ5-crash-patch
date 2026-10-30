// Reservation-log test: run as CivilizationV_restest.exe next to the probe's
// msimg32.dll with ReserveLog=1. Expect reserves_*.csv to list the 2 MB and
// 32 MB reservations (the 32 MB one released), the 4 MB heap big block, a 3 MB
// reservation placed at a chosen address (type 0x40002000), and the heap segments
// added while 64 MB of 100 KB blocks are allocated, each with a stack through
// make_* below.
// With an argument N it also leaves a fragmented CRT heap and waits N seconds,
// for the HeapHistogram check at the 60 s snapshot (AllocTracking=1, AllocCrt=ucrtbase.dll).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

__attribute__((noinline)) static void *make_reserve(SIZE_T n) { return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_NOACCESS); }
__attribute__((noinline)) static void *make_big(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 0, n); }
__attribute__((noinline)) static void *make_placed(void *at, SIZE_T n) { return VirtualAlloc(at, n, MEM_RESERVE, PAGE_NOACCESS); }
// A stack value pointing into a DLL's code while the DLL is loaded, and again
// after it is unloaded (the game's startup does this with GPU driver DLLs).
__attribute__((noinline)) static void reserve_with(ULONG_PTR stale)
{
    volatile ULONG_PTR keep[4] = {stale, stale, stale, stale};
    VirtualFree(make_reserve(2 << 20), 0, MEM_RELEASE);
    (void)keep[0];
}

__attribute__((noinline)) static void grow_heap(HANDLE h)
{
    for (int i = 0; i < 640; i++) HeapAlloc(h, 0, 100 * 1024);
}

int main(int argc, char **argv)
{
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    HMODULE dll = LoadLibraryA("dbghelp.dll");
    ULONG_PTR code = dll ? (ULONG_PTR)GetProcAddress(dll, "SymInitialize") + 0x10 : 0;
    reserve_with(code);
    FreeLibrary(dll);
    reserve_with(code);
    printf("unloaded-DLL case passed (dbghelp %s)\n", GetModuleHandleA("dbghelp.dll") ? "still loaded" : "unloaded");
    void *a = make_reserve(2 << 20);
    void *b = make_reserve(32 << 20);
    VirtualFree(b, 0, MEM_RELEASE);
    void *c = make_big(4 << 20);
    void *at = make_reserve(8 << 20);  // a free address for the placed reservation
    VirtualFree(at, 0, MEM_RELEASE);
    void *d = make_placed((BYTE *)at + (1 << 20), 3 << 20);
    HANDLE h = HeapCreate(0, 0, 0);
    grow_heap(h);
    printf("a=%p b=%p c=%p d=%p heap=%p\n", a, b, c, d, (void *)h);
    int wait = argc > 1 ? atoi(argv[1]) : 0;
    if (wait) {
        static void *blocks[20000];
        for (int i = 0; i < 20000; i++) blocks[i] = malloc(64 + (i % 7) * 900);
        for (int i = 0; i < 20000; i += 2) free(blocks[i]);  // every other block: small holes
    }
    Sleep(wait ? wait * 1000 : 5000);  // the sampler drains every 2 s
    return 0;
}
