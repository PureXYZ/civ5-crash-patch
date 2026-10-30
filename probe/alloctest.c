// Allocation-tracking test for civprobe. Built as CivilizationV_alloctest.exe and
// run with AllocCrt=ucrtbase.dll; prints what each site should show.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define NOINLINE __attribute__((noinline))

static void *keep[1 << 20];
static int nkeep;

NOINLINE static void big_site(void) { for (int i = 0; i < 50; i++) keep[nkeep++] = malloc(1 << 20); }
NOINLINE static void small_site(void) { for (int i = 0; i < 400000; i++) keep[nkeep++] = malloc(64); }
NOINLINE static void *wrapper_new(size_t n) { return malloc(n); }  // stands in for an operator new
NOINLINE static void caller_a(void) { for (int i = 0; i < 200; i++) keep[nkeep++] = wrapper_new(100 << 10); }
NOINLINE static void caller_b(void) { for (int i = 0; i < 100; i++) keep[nkeep++] = wrapper_new(100 << 10); }
NOINLINE static void churn_site(void) { for (int i = 0; i < 100000; i++) free(malloc(1000)); }
NOINLINE static void realloc_site(void)
{
    void *p = NULL;
    for (size_t n = 4096; n <= (8u << 20); n *= 2) p = realloc(p, n);
    keep[nkeep++] = p;
}
NOINLINE static void calloc_site(void) { keep[nkeep++] = calloc(5, 1 << 20); }

int main(void)
{
    // Load the probe (msimg32 proxy) through a real import.
    HDC dc = CreateCompatibleDC(NULL);
    TRIVERTEX v[2] = {{0, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0}};
    GRADIENT_RECT gr = {0, 1};
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);

    int smallStart = nkeep;
    big_site();
    smallStart = nkeep;
    small_site();
    int smallEnd = nkeep;
    caller_a();
    caller_b();
    churn_site();
    realloc_site();
    calloc_site();
    for (int i = smallStart; i < smallEnd; i += 2) { free(keep[i]); keep[i] = NULL; }  // free half

    printf("expected live: big_site 50.0 MB exact | small_site ~12.2 MB (200000 x 64 B, sampled)\n");
    printf("               wrapper_new via caller_a 19.5 MB, via caller_b 9.8 MB | churn 0 (100000 calls)\n");
    printf("               realloc 8.0 MB | calloc 5.0 MB\n");
    printf("sites: big_site=%p small_site=%p wrapper=%p caller_a=%p caller_b=%p\n",
           (void *)big_site, (void *)small_site, (void *)wrapper_new, (void *)caller_a, (void *)caller_b);
    Sleep(3500);
    return 0;
}
