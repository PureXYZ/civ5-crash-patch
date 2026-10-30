// Audio guard test: run as CivilizationV_audiotest.exe next to the probe's msimg32.dll
// and the fake mss32.dll. Calls on an allocated sample reach "Miles"; calls on a handle the
// game never got from Miles (a forged one that looks valid, like HSAMPLE 0x2F1E4F10 in session
// 20261028_235227), on a released one, or on garbage must return at once (status = SMP_DONE)
// instead of reaching Miles, with the stack balanced; a second release is skipped too. A skipped
// getter that answers through pointers (AIL_sample_volume_pan) leaves zeros there.
#include <windows.h>
#include <stdio.h>
typedef struct { int magic; int *driver; int index; } Sample;
__declspec(dllimport) int __stdcall AIL_sample_status(Sample *);
__declspec(dllimport) void __stdcall AIL_set_sample_3D_position(Sample *, float, float, float);
__declspec(dllimport) Sample *__stdcall AIL_allocate_sample_handle(void *);
__declspec(dllimport) void __stdcall AIL_release_sample_handle(Sample *);
__declspec(dllimport) void __stdcall AIL_sample_volume_pan(Sample *, float *, float *);
__declspec(dllimport) int __stdcall fake_calls(void);
__declspec(dllimport) int __stdcall fake_slot(void);

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    int table[4] = {0, 1, 0, 0};
    Sample forged = {1, table, 1};
    Sample *s1 = AIL_allocate_sample_handle((void *)1);
    int a = AIL_sample_status(s1);
    AIL_set_sample_3D_position(s1, 1, 2, 3);
    int calls = fake_calls();                       // alloc, status, 3D
    int f = AIL_sample_status(&forged);             // looks valid, never allocated
    AIL_set_sample_3D_position(&forged, 1, 2, 3);
    AIL_release_sample_handle(s1);
    int b = AIL_sample_status(s1);                  // released
    int b2 = AIL_sample_status((Sample *)0xFFFFFFFF);  // garbage, as in session 233828
    float vol = 7.0f, pan = 7.0f;                   // as the game's stack might hold
    AIL_sample_volume_pan(s1, &vol, &pan);          // released: skipped, zeros
    AIL_release_sample_handle(s1);                  // second release: skipped
    Sample *c = AIL_allocate_sample_handle(NULL);
    int after = fake_calls();                       // + release + alloc
    volatile int sp_check = 12345;                  // stack must be balanced after the skipped stdcalls
    printf("live status %d (4), slot %d (6), forged status %d (2), forged table %d (1), released %d (2), garbage %d (2), real calls %d then %d (3, 5), alloc %p (0), stack %d, skipped volume/pan %g/%g (0/0)\n",
           a, fake_slot(), f, table[1], b, b2, calls, after, (void *)c, sp_check, vol, pan);
    int ok = s1 && a == 4 && fake_slot() == 6 && f == 2 && table[1] == 1 && b == 2 && b2 == 2 && calls == 3 && after == 5 && !c && sp_check == 12345 &&
             vol == 0.0f && pan == 0.0f;
    printf("%s\n", ok ? "ALL PASSED" : "FAILURES");
    Sleep(2500);  // let the sampler report the skips
    return ok ? 0 : 1;
}