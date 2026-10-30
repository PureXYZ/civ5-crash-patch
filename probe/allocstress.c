// Multithreaded stress test for civprobe's allocation hooks: 8 threads doing
// random malloc/realloc/free; reports the true live bytes for comparison.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define THREADS 8
#define SLOTS 4096
#define OPS 400000

static size_t g_live[THREADS];

static DWORD WINAPI worker(LPVOID arg)
{
    int id = (int)(INT_PTR)arg;
    void *ptr[SLOTS] = {0};
    size_t size[SLOTS] = {0};
    unsigned x = 12345u + id * 7919u;
    for (int i = 0; i < OPS; i++) {
        x = x * 1103515245u + 12345u;
        int s = (x >> 8) % SLOTS;
        size_t n = (x >> 4) % 4 == 0 ? 4096 + ((x >> 12) % 60000) : 16 + ((x >> 12) % 2000);
        switch ((x >> 20) % 3) {
        case 0: free(ptr[s]); ptr[s] = malloc(n); size[s] = n; break;
        case 1: { void *q = realloc(ptr[s], n); if (q) { ptr[s] = q; size[s] = n; } break; }
        case 2: free(ptr[s]); ptr[s] = NULL; size[s] = 0; break;
        }
    }
    size_t live = 0;
    for (int s = 0; s < SLOTS; s++) live += size[s];
    g_live[id] = live;
    Sleep(4000);  // keep blocks alive until the probe's report
    return 0;
}

int main(void)
{
    HDC dc = CreateCompatibleDC(NULL);
    TRIVERTEX v[2] = {{0, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0}};
    GRADIENT_RECT gr = {0, 1};
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);

    DWORD t0 = GetTickCount();
    HANDLE th[THREADS];
    for (int i = 0; i < THREADS; i++) th[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, NULL);
    Sleep(1500);
    size_t live = 0;
    for (int i = 0; i < THREADS; i++) live += g_live[i];
    printf("ops done in ~%lu ms, true live = %.1f MB\n", GetTickCount() - t0, live / 1048576.0);
    WaitForMultipleObjects(THREADS, th, TRUE, INFINITE);
    return 0;
}
