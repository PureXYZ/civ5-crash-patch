// Command stream slack test: run as CivilizationV_slacktest.exe with
// CommandStreamTestRets set to the return addresses of the two calls marked below (the
// build prints them). Blocks from those calls must be 4 KB bigger; others unchanged.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

__attribute__((noinline)) void *helper_aligned(size_t n) { void *volatile p = _aligned_malloc(n, 16); return p; }
__attribute__((noinline)) void *stream_like_malloc(size_t n) { void *volatile p = malloc(n); return p; }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    void *a = stream_like_malloc(100000);      // marked call 1
    void *b = helper_aligned(2u << 20);        // marked call 2
    void *c = malloc(100000);                  // not marked
    size_t sa = _msize(a), sb = _aligned_msize(b, 16, 0), sc = _msize(c);
    printf("marked malloc %zu (expect 104096), marked aligned %zu (expect 2101248), other %zu (expect 100000)\n", sa, sb, sc);
    int ok = sa == 104096 && sb == 2101248 && sc == 100000;
    printf("%s\n", ok ? "ALL PASSED" : "FAILURES");
    free(a); _aligned_free(b); free(c);
    return ok ? 0 : 1;
}
