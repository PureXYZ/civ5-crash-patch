// VA-map label test: run as CivilizationV_milabel.exe next to the probe with MiMalloc=1 and
// Telemetry=1. It keeps 20 MB of small msvcr90 blocks live past the 60 s snapshot, so
// civprobe\vamap_<session>_t60s.csv must name their segments "mimalloc".
#include <windows.h>
#include <stdio.h>

extern void *(__cdecl *const imp_malloc)(size_t) __asm__("__imp__malloc");  // msvcr90, like the game
extern void (__cdecl *const imp_free)(void *) __asm__("__imp__free");

int main(void)
{
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    static void *blocks[20000];
    for (int i = 0; i < 20000; i++) blocks[i] = imp_malloc(1000);
    printf("holding 20000 blocks (first %p); waiting for the 60 s snapshot\n", blocks[0]);
    Sleep(65000);
    for (int i = 0; i < 20000; i++) imp_free(blocks[i]);
    return 0;
}
