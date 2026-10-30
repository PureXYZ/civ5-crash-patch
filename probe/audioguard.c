// Guard against invalid Miles sound sample handles ([probe] AudioGuard=1, the default).
//
// The audio engine's sample table (FAudioSystemMiles, {flag, HSAMPLE, flag} per slot) was
// being overwritten by the render command stream's overflowing command array (see
// cmdstream.c), so the game passed Miles handles that were 0xFFFFFFFF, released samples, or
// pointers into render data (HSAMPLE 0x2F1E4F10 with "driver" 32.0f, session 20261028_235227),
// and Miles crashed (AIL_sample_status, AIL_stop_sample, AIL_set_sample_volume_pan).
// cmdstream.c fixes the overflow; this is the second line of defence.
//
// The exe's AIL_allocate_sample_handle / AIL_stream_sample_handle results are recorded as
// live, AIL_release_sample_handle / AIL_close_stream (its sample) / AIL_close_digital_driver
// (all) take them out again. Every exe import from mss32.dll whose first parameter is an
// HSAMPLE (the *sample* functions and AIL_register_EOS_callback; the @N in the import name
// gives the stdcall argument size) goes through a generated thunk that calls sample_ok: a
// handle that is not live (or, without tracking, not a plausible pointer or with a NULL
// driver field) makes the call return at once (AIL_sample_status reports SMP_DONE so the game
// finishes the sound; the rest return 0, and the getters that answer through pointers get zeros
// written there, so the game never reads what its stack held). NULL goes to Miles, which handles it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "probe.h"

#define MAX_GUARDED 48
#define THUNK 56
#define SMP_DONE 2

static void *g_real[MAX_GUARDED];        // jmp [g_real[i]] targets
static volatile LONG g_hits[MAX_GUARDED];
static char g_names[MAX_GUARDED][48];
static int g_n;
static LONG g_reported[MAX_GUARDED];

// ---- Live handles ----

#define MAX_LIVE 1024
#define MAX_STREAMS 64
static void *g_live[MAX_LIVE];
static int g_nLive;
static struct { void *stream, *sample; } g_streams[MAX_STREAMS];
static volatile LONG g_liveLock;
static int g_tracking;
static volatile LONG g_trackingLost;  // a table filled up: back to the plausible-handle check

static void live_lock(void) { while (InterlockedExchange(&g_liveLock, 1)) YieldProcessor(); }
static void live_unlock(void) { InterlockedExchange(&g_liveLock, 0); }

static int live_index(void *h)
{
    for (int i = 0; i < g_nLive; i++)
        if (g_live[i] == h) return i;
    return -1;
}

static void live_add(void *h)
{
    live_lock();
    if (live_index(h) < 0) {
        if (g_nLive < MAX_LIVE) g_live[g_nLive++] = h;
        else if (g_tracking) g_tracking = 0, g_trackingLost = 1;  // else this live sample would be skipped
    }
    live_unlock();
}

static void live_remove(void *h)
{
    live_lock();
    int i = live_index(h);
    if (i >= 0) g_live[i] = g_live[--g_nLive];
    live_unlock();
}

// cdecl, called from the thunks with the handle (the first argument).
int __cdecl sample_ok(void *h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if (!v) return 1;
    if (v < 0x10000 || v >= 0xFFFF0000u) return 0;
    if (g_tracking) {
        live_lock();
        int known = live_index(h) >= 0;
        live_unlock();
        if (!known) return 0;
    } else {  // untracked: h may be any number (a record pointer was seen)
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(h, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            (const BYTE *)h + 8 > (const BYTE *)m.BaseAddress + m.RegionSize)
            return 0;
    }
    return ((void **)h)[1] != NULL;  // the driver field; Miles clears it on release
}

typedef void *(__stdcall *h1_t)(void *);
typedef void(__stdcall *v1_t)(void *);
static h1_t o_alloc, o_streamSample;
static v1_t o_release, o_closeStream, o_closeDriver;

static void *__stdcall w_alloc(void *driver)
{
    void *h = o_alloc(driver);
    if (h) live_add(h);
    return h;
}

static volatile LONG g_badReleases;

static void __stdcall w_release(void *h)
{
    if (h && !sample_ok(h)) { InterlockedIncrement(&g_badReleases); return; }
    live_remove(h);
    o_release(h);
}

static void *__stdcall w_streamSample(void *stream)
{
    void *h = o_streamSample(stream);
    if (h) {
        live_add(h);
        live_lock();
        int free = -1;
        for (int i = 0; i < MAX_STREAMS; i++) {
            if (g_streams[i].stream == stream) { free = i; break; }
            if (free < 0 && !g_streams[i].stream) free = i;
        }
        if (free >= 0) { g_streams[free].stream = stream; g_streams[free].sample = h; }
        else if (g_tracking) g_tracking = 0, g_trackingLost = 1;  // its sample's end couldn't be followed
        live_unlock();
    }
    return h;
}

static void __stdcall w_closeStream(void *stream)
{
    void *h = NULL;
    live_lock();
    for (int i = 0; i < MAX_STREAMS; i++)
        if (g_streams[i].stream == stream) { h = g_streams[i].sample; g_streams[i].stream = NULL; break; }
    live_unlock();
    if (h) live_remove(h);
    o_closeStream(stream);
}

static void __stdcall w_closeDriver(void *driver)
{
    live_lock();  // its samples go with it
    g_nLive = 0;
    memset(g_streams, 0, sizeof g_streams);
    live_unlock();
    o_closeDriver(driver);
}

// ---- Thunks ----

static int takes_sample_first(const char *name)
{
    if (strncmp(name, "_AIL_", 5) != 0) return 0;
    if (strstr(name, "allocate_sample_handle") || strstr(name, "stream_sample_handle") || strstr(name, "release_sample_handle")) return 0;
    return strstr(name, "sample") != NULL || strstr(name, "register_EOS_callback") != NULL;
}

// Getters that answer through pointers (arguments 2..1+n): a skipped call zeros those.
static int out_pointers(const char *name)
{
    static const struct { const char *name; int n; } k[] = {
        {"_AIL_sample_volume_pan@", 2}, {"_AIL_sample_ms_position@", 2},
        {"_AIL_sample_reverb_levels@", 2}, {"_AIL_sample_51_volume_levels@", 6}};
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strncmp(name, k[i].name, strlen(k[i].name))) return k[i].n;
    return 0;
}

// cdecl, from a thunk's skip path: args = the caller's second argument.
void __cdecl zero_outs(DWORD *const *args, int n)
{
    for (int i = 0; i < n; i++)
        if (args[i]) *args[i] = 0;  // 0 is 0.0f too
}

// mov eax,[esp+4]; push eax; call sample_ok; add esp,4; test eax,eax; jnz real;
// lock inc [hits]; [lea eax,[esp+8]; push outs; push eax; call zero_outs; add esp,8;]
// mov eax,ret; ret N; real: jmp [g_real[i]]
static void emit(BYTE *t, int i, DWORD argBytes, DWORD ret, int outs)
{
    BYTE *p = t;
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x04;  // mov eax,[esp+4]
    *p++ = 0x50;                                         // push eax
    *p++ = 0xE8;                                         // call sample_ok
    LONG rel = (LONG)((BYTE *)sample_ok - (p + 4)); memcpy(p, &rel, 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;               // add esp,4
    *p++ = 0x85; *p++ = 0xC0;                            // test eax,eax
    BYTE *jnz = p; *p++ = 0x75; *p++ = 0;                // jnz real
    *p++ = 0xF0; *p++ = 0xFF; *p++ = 0x05;               // lock inc dword [hits]
    DWORD a = (DWORD)(ULONG_PTR)&g_hits[i]; memcpy(p, &a, 4); p += 4;
    if (outs) {
        *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x08;  // lea eax,[esp+8]
        *p++ = 0x6A; *p++ = (BYTE)outs;                      // push outs
        *p++ = 0x50;                                         // push eax
        *p++ = 0xE8;                                         // call zero_outs
        rel = (LONG)((BYTE *)zero_outs - (p + 4)); memcpy(p, &rel, 4); p += 4;
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;               // add esp,8
    }
    *p++ = 0xB8; memcpy(p, &ret, 4); p += 4;             // mov eax,ret
    *p++ = 0xC2; *p++ = (BYTE)argBytes; *p++ = (BYTE)(argBytes >> 8);  // ret N
    BYTE *real = p;
    *p++ = 0xFF; *p++ = 0x25;                            // jmp [g_real[i]]
    a = (DWORD)(ULONG_PTR)&g_real[i]; memcpy(p, &a, 4); p += 4;
    jnz[1] = (BYTE)(real - (jnz + 2));
    _Static_assert(THUNK >= 53, "thunk size");
}

static void set_slot(IMAGE_THUNK_DATA32 *slot, void *to)
{
    DWORD old;
    if (VirtualProtect(&slot->u1.Function, 4, PAGE_READWRITE, &old)) {
        InterlockedExchange((volatile LONG *)&slot->u1.Function, (LONG)(ULONG_PTR)to);
        VirtualProtect(&slot->u1.Function, 4, old, &old);
    }
}

void audioguard_init(const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"AudioGuard", 1, ini)) return;
    int track = GetPrivateProfileIntW(L"probe", L"AudioGuardTrack", 1, ini);
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    BYTE *page = NULL;
    IMAGE_THUNK_DATA32 *sAlloc = NULL, *sRelease = NULL, *sStream = NULL, *sClose = NULL, *sDriver = NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(exe + dir.VirtualAddress); dir.Size && d->Name; d++) {
        if (_stricmp((const char *)(exe + d->Name), "mss32.dll") != 0 || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA32 *names = (IMAGE_THUNK_DATA32 *)(exe + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA32 *slots = (IMAGE_THUNK_DATA32 *)(exe + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char *name = (const char *)((IMAGE_IMPORT_BY_NAME *)(exe + names->u1.AddressOfData))->Name;
            if (!strcmp(name, "_AIL_allocate_sample_handle@4")) { sAlloc = slots; continue; }
            if (!strcmp(name, "_AIL_release_sample_handle@4")) { sRelease = slots; continue; }
            if (!strcmp(name, "_AIL_stream_sample_handle@4")) { sStream = slots; continue; }
            if (!strcmp(name, "_AIL_close_stream@4")) { sClose = slots; continue; }
            if (!strcmp(name, "_AIL_close_digital_driver@4")) { sDriver = slots; continue; }
            const char *at = strrchr(name, '@');
            if (!at || !takes_sample_first(name)) continue;
            if (g_n >= MAX_GUARDED) { log_event("audioguard: more than %d sample functions; %s not guarded", MAX_GUARDED, name); continue; }
            DWORD argBytes = (DWORD)atoi(at + 1);
            if (!page) page = VirtualAlloc(NULL, MAX_GUARDED * THUNK, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!page) { log_event("audioguard: no thunk page; off"); return; }
            int i = g_n++;
            g_real[i] = (void *)(ULONG_PTR)slots->u1.Function;
            snprintf(g_names[i], sizeof g_names[i], "%.*s", (int)(at - name - 1), name + 1);
            int outs = out_pointers(name);
            if (outs && argBytes < 4u * (1 + outs)) outs = 0;
            emit(page + i * THUNK, i, argBytes, strstr(name, "_AIL_sample_status@") ? SMP_DONE : 0, outs);
            set_slot(slots, page + i * THUNK);
        }
    }
    if (!page) { log_event("audioguard: no mss32.dll sample imports in the exe; off"); return; }
    DWORD old;
    VirtualProtect(page, MAX_GUARDED * THUNK, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, MAX_GUARDED * THUNK);
    if (sRelease) {  // releasing an invalid handle is skipped too
        o_release = (v1_t)(ULONG_PTR)sRelease->u1.Function;
        set_slot(sRelease, (void *)w_release);
    }
    // Tracking needs every way the exe gets and loses a sample handle.
    if (track && sAlloc && sRelease) {
        o_alloc = (h1_t)(ULONG_PTR)sAlloc->u1.Function;
        if (sStream && sClose) {
            o_streamSample = (h1_t)(ULONG_PTR)sStream->u1.Function;
            o_closeStream = (v1_t)(ULONG_PTR)sClose->u1.Function;
            set_slot(sStream, (void *)w_streamSample);
            set_slot(sClose, (void *)w_closeStream);
        } else if (sStream) {
            track = 0;  // stream samples could not be followed
        }
        if (track) {
            if (sDriver) { o_closeDriver = (v1_t)(ULONG_PTR)sDriver->u1.Function; set_slot(sDriver, (void *)w_closeDriver); }
            set_slot(sAlloc, (void *)w_alloc);
            g_tracking = 1;
        }
    }
    log_event("audioguard: ON, %d Miles sample functions accept only %s", g_n,
              g_tracking ? "live sample handles (allocated and not released)" : "plausible handles with a driver");
}

// Sampler: logs which functions met an invalid handle since the last report.
void audioguard_tick(void)
{
    for (int i = 0; i < g_n; i++) {
        LONG h = g_hits[i];
        if (h != g_reported[i]) {
            log_event("audioguard: %s skipped %ld call(s) on a released or invalid sample (total %ld)", g_names[i], h - g_reported[i], h);
            g_reported[i] = h;
        }
    }
    if (g_trackingLost == 1) {
        g_trackingLost = 2;
        log_event("audioguard: more than %d live samples or %d streams; now only rejecting implausible handles", MAX_LIVE, MAX_STREAMS);
    }
    static LONG reportedReleases;
    if (g_badReleases != reportedReleases) {
        reportedReleases = g_badReleases;
        log_event("audioguard: AIL_release_sample_handle skipped on an invalid sample (total %ld)", reportedReleases);
    }
}