// Spike test for civprobe's "peak" alloc report: allocates ~320 MB in 64 KB blocks
// from one site, holds it, frees it, then idles so a later report sees it gone.
// Run with AllocCrt=ucrtbase.dll, a short IntervalMs and AllocReportSeconds.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCKS 5120

static __attribute__((noinline)) void *spike_alloc(size_t n) { return malloc(n); }

int main(void)
{
    HDC dc = CreateCompatibleDC(NULL);  // keep the msimg32 import
    TRIVERTEX v[2] = {{0, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0}};
    GRADIENT_RECT gr = {0, 1};
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);

    static void *keep[64];
    for (int i = 0; i < 64; i++) keep[i] = malloc(256 * 1024);  // 16 MB that stays live
    Sleep(2000);

    static void *p[BLOCKS];
    for (int i = 0; i < BLOCKS; i++) { p[i] = spike_alloc(64 * 1024); memset(p[i], 1, 64); }
    printf("spike: %d MB allocated by spike_alloc at %p\n", BLOCKS / 16, (void *)spike_alloc);
    Sleep(4000);
    for (int i = 0; i < BLOCKS; i++) free(p[i]);
    printf("spike freed; idling\n");
    Sleep(6000);
    printf("done (keep[0]=%p)\n", keep[0]);
    return 0;
}
