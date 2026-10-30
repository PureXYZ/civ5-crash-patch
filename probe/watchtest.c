// Watchpoint test: run as CivilizationV_watchtest.exe next to the probe's msimg32.dll
// with Watch=1, WatchGlobalExport=g_watched, IntervalMs=500 (page mode, the default;
// WatchMode=dr for the debug-register mode). The object comes from the exe's malloc
// import like FAudioGame. Its "vtable" is overwritten with 1 by a thread created after
// the watch was armed; the events log should show that WRITE with corrupt_object on
// the stack, count the write to field 1 (same page) without a stack, not fault on
// field 5 (other page), and log the FREE.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

__declspec(dllexport) void *volatile g_watched;

// Like the exe's new wrapper (exe+0x42c090): saves registers, then calls the malloc import.
__attribute__((noinline)) void *alloc_wrap(size_t n)
{
    void *volatile keep = NULL;
    void *p = malloc(n);
    keep = p;
    return keep;
}

__attribute__((noinline)) static void corrupt_object(volatile DWORD *obj) { obj[0] = 1; }
__attribute__((noinline)) static void touch_fields(volatile DWORD *obj) { obj[1] = 3; obj[5] = 7; }

static DWORD WINAPI late_thread(LPVOID arg)
{
    Sleep(1500);  // armed by now
    touch_fields(arg);
    corrupt_object(arg);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    volatile DWORD *obj = alloc_wrap(0x46C);  // no memset: clang would turn malloc+memset into calloc
    for (int i = 0; i < 0x46C / 4; i++) obj[i] = 0;
    obj[0] = 0x12345678;
    g_watched = (void *)obj;
    HANDLE t = CreateThread(NULL, 0, late_thread, (void *)obj, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    printf("object at %p: vtable 0x%lx (expect 1), field1 %lu (3), field5 %lu (7)\n", (void *)obj,
           (unsigned long)obj[0], (unsigned long)obj[1], (unsigned long)obj[5]);
    g_watched = NULL;
    free((void *)obj);
    Sleep(800);
    return obj && 1 ? 0 : 1;
}