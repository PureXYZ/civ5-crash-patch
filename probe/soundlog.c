// Sound log ([probe] SoundLog=1, diagnostic; off by default).
//
// Writes every sound the game starts to sounds_<session>.log, on the events log's clock, with the
// name the game gave Miles for it and its size in bytes, so a sound heard in game can be named,
// counted and matched with what the events log shows at that time. The game gives Miles only the
// file type (".wav", ".mp3") and streams get Miles memory-file names ("\\\\5604"); the size names
// the file: probe/soundnames.py matches it against the game's sound files. The exe loads a sound into a
// sample with AIL_set_named_sample_file (one call site, in the wrapper at exe+0x523180) and starts
// it with AIL_start_sample; streamed sounds (music, ambience) are opened by name with
// AIL_open_stream (wrapper at exe+0x5237f0) and started with AIL_start_stream. Those four import
// slots are wrapped, after the audio guard has put its thunks in place (a wrapper then calls the
// thunk). A load or open that fails is logged with AIL_last_error. No game code is changed.
// The periodic and exit reports in the events log list the most started sounds and the most
// starts of each within one second.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

#define MAX_SOUNDS 1024   // distinct (name, size); the last entry collects the rest
#define MAX_HANDLES 4096  // sample and stream handles -> sound, direct-mapped (a collision forgets the older one)
#define NAME 96
#define TOP 10

struct sound {
    char name[NAME];
    DWORD size;
    LONG starts, winStarts, peak;
    double winStart, peakAt;
};
static struct sound g_sounds[MAX_SOUNDS];
static int g_nSounds;
static struct { void *h; int sound; } g_handles[MAX_HANDLES];
static LONG g_starts, g_streams, g_unknown, g_failed;
static volatile LONG g_lock;
static HANDLE g_out = INVALID_HANDLE_VALUE;

typedef int(__stdcall *load_t)(void *, const char *, const void *, DWORD, int);
typedef void(__stdcall *start_t)(void *);
typedef void *(__stdcall *open_t)(void *, const char *, int);
typedef const char *(__stdcall *error_t)(void);
static load_t o_load;
static start_t o_startSample, o_startStream;
static open_t o_open;
static error_t o_error;

static void lock(void) { while (InterlockedExchange(&g_lock, 1)) YieldProcessor(); }
static void unlock(void) { InterlockedExchange(&g_lock, 0); }

static int slot_of(void *h) { return (int)(((ULONG_PTR)h >> 4) % MAX_HANDLES); }

// The end of long names: that's where the file name is.
static void copy_name(char *to, const char *name)
{
    if (!name) name = "(no name)";
    size_t n = strlen(name);
    if (n >= NAME) name += n - (NAME - 1);
    lstrcpynA(to, name, NAME);
}

// Under the lock.
static int sound_of(const char *name, DWORD size)
{
    char key[NAME];
    copy_name(key, name);
    for (int i = 0; i < g_nSounds; i++)
        if (g_sounds[i].size == size && !strcmp(g_sounds[i].name, key)) return i;
    if (g_nSounds == MAX_SOUNDS - 1) {
        lstrcpynA(g_sounds[g_nSounds].name, "(more sounds than the log tracks)", NAME);
        g_nSounds++;
    }
    if (g_nSounds == MAX_SOUNDS) return MAX_SOUNDS - 1;
    struct sound *s = &g_sounds[g_nSounds];
    lstrcpyA(s->name, key);
    s->size = size;
    return g_nSounds++;
}

static void bind(void *h, int sound)
{
    int i = slot_of(h);
    g_handles[i].h = h;
    g_handles[i].sound = sound;
}

static void note_start(void *h, const char *kind)
{
    double t = log_time_s();
    char line[256];
    int n;
    lock();
    int i = slot_of(h);
    if (h && g_handles[i].h == h) {
        struct sound *s = &g_sounds[g_handles[i].sound];
        s->starts++;
        if (t - s->winStart >= 1.0) s->winStart = t, s->winStarts = 0;
        if (++s->winStarts > s->peak) s->peak = s->winStarts, s->peakAt = s->winStart;
        char size[24] = "";
        if (s->size) snprintf(size, sizeof size, " (%lu bytes)", s->size);
        n = snprintf(line, sizeof line, "[%9.2f] %s %s%s\r\n", t, kind, s->name, size);
    } else {
        g_unknown++;
        n = snprintf(line, sizeof line, "[%9.2f] %s on %p, which no logged load or open gave a sound\r\n", t, kind, h);
    }
    g_starts++;
    unlock();
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    write_all(g_out, line, n);
}

static void note_failure(const char *what, const char *name, DWORD size)
{
    char line[384], key[NAME], bytes[24] = "";
    copy_name(key, name);
    if (size) snprintf(bytes, sizeof bytes, " (%lu bytes)", size);
    const char *err = o_error ? o_error() : NULL;
    int n = snprintf(line, sizeof line, "[%9.2f] %s failed: %s%s: %.200s\r\n", log_time_s(), what, key, bytes,
                     err && *err ? err : "no error text");
    InterlockedIncrement(&g_failed);
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    write_all(g_out, line, n);
}

static int __stdcall w_load(void *sample, const char *name, const void *image, DWORD size, int block)
{
    int ok = o_load(sample, name, image, size, block);
    if (ok) {
        lock();
        bind(sample, sound_of(name, size));
        unlock();
    } else
        note_failure("load", name, size);
    return ok;
}

static void __stdcall w_startSample(void *sample)
{
    note_start(sample, "play");
    o_startSample(sample);
}

static void *__stdcall w_open(void *driver, const char *name, int mem)
{
    void *stream = o_open(driver, name, mem);
    if (stream) {
        lock();
        bind(stream, sound_of(name, 0));
        unlock();
    } else
        note_failure("stream open", name, 0);
    return stream;
}

static void __stdcall w_startStream(void *stream)
{
    InterlockedIncrement(&g_streams);
    note_start(stream, "stream");
    o_startStream(stream);
}

static int hook(BYTE *exe, const char *name, void *wrapper, void **orig)
{
    void *volatile *slot = (void *volatile *)exe_import_slot(exe, name);
    DWORD old;
    if (!slot || !*slot || !VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old)) return 0;
    *orig = *slot;
    InterlockedExchange((volatile LONG *)slot, (LONG)(ULONG_PTR)wrapper);
    VirtualProtect((void *)slot, 4, old, &old);
    return 1;
}

void soundlog_init(const WCHAR *ini)
{
    if (GetPrivateProfileIntW(L"probe", L"SoundLog", 0, ini) != 1) return;
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    void *volatile *err = (void *volatile *)exe_import_slot(exe, "_AIL_last_error@0");
    if (!exe_import_slot(exe, "_AIL_set_named_sample_file@20") || !exe_import_slot(exe, "_AIL_start_sample@4")) {
        log_event("soundlog: the exe doesn't import AIL_set_named_sample_file and AIL_start_sample; off");
        return;
    }
    WCHAR name[64];
    _snwprintf(name, 64, L"sounds_%s.log", g_session);
    name[63] = 0;
    g_out = open_out(name);
    if (g_out == INVALID_HANDLE_VALUE) {
        log_event("soundlog: can't create %ls; off", name);
        return;
    }
    if (err) o_error = (error_t)*err;
    int n = hook(exe, "_AIL_set_named_sample_file@20", (void *)w_load, (void **)&o_load);
    n += hook(exe, "_AIL_start_sample@4", (void *)w_startSample, (void **)&o_startSample);
    int streams = hook(exe, "_AIL_open_stream@12", (void *)w_open, (void **)&o_open);
    streams += hook(exe, "_AIL_start_stream@4", (void *)w_startStream, (void **)&o_startStream);
    log_event("soundlog: every sound the game starts goes to %ls (%d of 2 sample calls, %d of 2 stream calls wrapped); "
              "diagnostic, no game code changed", name, n, streams);
}

void soundlog_report(void)
{
    if (g_out == INVALID_HANDLE_VALUE) return;
    struct sound top[TOP];
    int nTop = 0, distinct;
    lock();
    distinct = g_nSounds;
    BYTE done[MAX_SOUNDS] = {0};
    for (; nTop < TOP; nTop++) {
        int best = -1;
        for (int i = 0; i < g_nSounds; i++)
            if (!done[i] && g_sounds[i].starts && (best < 0 || g_sounds[i].starts > g_sounds[best].starts)) best = i;
        if (best < 0) break;
        done[best] = 1;
        top[nTop] = g_sounds[best];
    }
    unlock();
    log_event("soundlog: %ld sounds started (%ld of them streams) from %d different files; %ld on a handle with no "
              "logged load, %ld loads or opens failed", g_starts, g_streams, distinct, g_unknown, g_failed);
    for (int i = 0; i < nTop; i++) {
        char size[24] = "";
        if (top[i].size) snprintf(size, sizeof size, " (%lu bytes)", top[i].size);
        log_event("soundlog:   %s%s: %ld starts, up to %ld within 1 s (at %.1f s)", top[i].name, size, top[i].starts,
                  top[i].peak, top[i].peakAt);
    }
}
