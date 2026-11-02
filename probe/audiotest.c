// Audio guard test: run as CivilizationV_audiotest.exe next to the probe's msimg32.dll
// and the fake mss32.dll. Calls on an allocated sample reach "Miles"; calls on a handle the
// game never got from Miles (a forged one that looks valid, like HSAMPLE 0x2F1E4F10 in session
// 20261028_235227), on a released one, or on garbage must return at once (status = SMP_DONE)
// instead of reaching Miles, with the stack balanced; a second release is skipped too. A skipped
// getter that answers through pointers (AIL_sample_volume_pan) leaves zeros there.
// With SoundLog=1: loads, plays, failed loads and streams must reach "Miles" and show up in this
// run's sounds_<session>.log (its sound names carry the process id).
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef struct { int magic; int *driver; int index; } Sample;
__declspec(dllimport) int __stdcall AIL_sample_status(Sample *);
__declspec(dllimport) void __stdcall AIL_set_sample_3D_position(Sample *, float, float, float);
__declspec(dllimport) Sample *__stdcall AIL_allocate_sample_handle(void *);
__declspec(dllimport) void __stdcall AIL_release_sample_handle(Sample *);
__declspec(dllimport) void __stdcall AIL_sample_volume_pan(Sample *, float *, float *);
__declspec(dllimport) int __stdcall AIL_set_named_sample_file(Sample *, const char *, const void *, unsigned, int);
__declspec(dllimport) void __stdcall AIL_start_sample(Sample *);
__declspec(dllimport) void *__stdcall AIL_open_stream(void *, const char *, int);
__declspec(dllimport) void __stdcall AIL_start_stream(void *);
__declspec(dllimport) const char *__stdcall AIL_last_error(void);  // imported, as by the game, for the failure lines
__declspec(dllimport) int __stdcall fake_calls(void);
__declspec(dllimport) int __stdcall fake_started(void);
__declspec(dllimport) int __stdcall fake_slot(void);

// This run's sound log: the sounds_*.log in civprobe that holds marker. Returns its text in buf.
static int read_sound_log(const char *marker, char *buf, int cap)
{
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA("civprobe\\sounds_*.log", &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "civprobe\\%s", fd.cFileName);
        FILE *in = fopen(path, "rb");
        if (!in) continue;
        int n = (int)fread(buf, 1, cap - 1, in);
        fclose(in);
        buf[n > 0 ? n : 0] = 0;
        found = strstr(buf, marker) != NULL;
    } while (!found && FindNextFileA(f, &fd));
    FindClose(f);
    return found;
}

static int count(const char *text, const char *what)
{
    int n = 0;
    for (const char *p = text; (p = strstr(p, what)); p++) n++;
    return n;
}

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
    // Sound log: three plays of a loaded sample, a failed load, a stream, a failed open, a start
    // on a handle nothing loaded.
    char ding[64], theme[64], line[96], log[8192];
    snprintf(ding, sizeof ding, "Sounds\\Test\\Ding_%lu.ogg", GetCurrentProcessId());
    snprintf(theme, sizeof theme, "Music\\Theme_%lu.mp3", GetCurrentProcessId());
    static const char image[1234];
    Sample *s2 = AIL_allocate_sample_handle((void *)1);
    int loaded = AIL_set_named_sample_file(s2, ding, image, sizeof image, 0);
    for (int i = 0; i < 3; i++) AIL_start_sample(s2);
    int failed = AIL_set_named_sample_file(s2, "Missing.ogg", image, 0, 0);
    void *stream = AIL_open_stream(NULL, theme, 0);
    AIL_start_stream(stream);
    void *noStream = AIL_open_stream(NULL, "", 0);
    AIL_start_stream((void *)0x12345670);
    int started = fake_started() + (strcmp(AIL_last_error(), "fake error") ? 100 : 0);
    int logged = read_sound_log(ding, log, sizeof log);
    snprintf(line, sizeof line, "play %s (1234 bytes)", ding);
    int plays = count(log, line);
    snprintf(line, sizeof line, "stream %s\r\n", theme);
    int streams = count(log, line);
    int failures = count(log, "load failed: Missing.ogg: fake error") + count(log, "stream open failed: : fake error");
    int unknown = count(log, "stream on 12345670, which no logged load or open gave a sound");
    printf("sound log: loaded %d (1), failed %d (0), stream %s, no stream %s, started %d (5), log found %d, plays %d (3), "
           "streams %d (1), failures %d (2), unknown %d (1)\n", loaded, failed, stream ? "yes" : "no", noStream ? "yes" : "no",
           started, logged, plays, streams, failures, unknown);
    int soundOk = loaded == 1 && failed == 0 && stream && !noStream && started == 5 && logged && plays == 3 && streams == 1 &&
                  failures == 2 && unknown == 1;
    volatile int sp_check = 12345;                  // stack must be balanced after the skipped stdcalls
    printf("live status %d (4), slot %d (6), forged status %d (2), forged table %d (1), released %d (2), garbage %d (2), real calls %d then %d (3, 5), alloc %p (0), stack %d, skipped volume/pan %g/%g (0/0)\n",
           a, fake_slot(), f, table[1], b, b2, calls, after, (void *)c, sp_check, vol, pan);
    int ok = s1 && a == 4 && fake_slot() == 6 && f == 2 && table[1] == 1 && b == 2 && b2 == 2 && calls == 3 && after == 5 && !c && sp_check == 12345 &&
             vol == 0.0f && pan == 0.0f && soundOk;
    printf("%s\n", ok ? "ALL PASSED" : "FAILURES");
    Sleep(2500);  // let the sampler report the skips
    return ok ? 0 : 1;
}