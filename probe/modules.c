// Module list for the events log, and the stack scan for logged exceptions.
//
// The events log gets every DLL from outside the Windows folder, with its base, size and PE
// timestamp. Exceptions are logged as module+offset, and for a mod's DLL (Vox Populi's
// CvGameCore_Expansion2.dll changes with each release) only the timestamp says which build
// that offset belongs to. Startup lists what is already loaded (the exe's imports); a loader
// notification reports the rest as they load and unload. Overlays and injected DLLs (Steam,
// Discord, ReShade, antivirus hooks) show up here too. Paths under the user profile are
// written as %USERPROFILE%.
//
// The same list, system DLLs included, fills a table of module ranges that the exception path
// reads without locks: GetModuleHandleEx takes the loader lock, which another thread may hold
// when the game faults. The stack scan reads the faulting thread's stack upward from Esp and
// logs each value that points into a module's code just after a call instruction - return
// address candidates, as in Vox Populi's crashes.log. Stale values left in a frame also pass,
// so the list is read innermost first and anything that doesn't fit is skipped.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "probe.h"

BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE *, DWORD, LPDWORD);

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UStr;
typedef struct { ULONG Flags; const UStr *FullDllName; const UStr *BaseDllName; void *DllBase; ULONG SizeOfImage; } DllNotifyData;
typedef VOID(CALLBACK *DllNotify_t)(ULONG reason, const DllNotifyData *data, void *ctx);
typedef LONG(NTAPI *LdrRegisterDllNotification_t)(ULONG flags, DllNotify_t fn, void *ctx, void **cookie);
#define DLL_NOTIFY_LOADED 1

static WCHAR g_winDir[MAX_PATH];
static int g_winDirLen;
static WCHAR g_profile[MAX_PATH];
static int g_profileLen;

/* ------------------------------------------------------------------ */
/* Module table (written under the loader lock, read without locks)    */
/* ------------------------------------------------------------------ */

typedef struct {
    volatile ULONG_PTR base;   // 0 = free slot; set last, cleared first
    ULONG_PTR end;
    ULONG_PTR codeLo, codeHi;  // span of the executable sections
    char name[64];
} Mod;
#define MAX_MODS 512
static Mod g_mods[MAX_MODS];
static volatile LONG g_modCount;  // slots in use or freed, never shrinks

static void table_add(const BYTE *base, DWORD size, const WCHAR *path, int len)
{
    int i;
    for (i = 0; i < g_modCount && g_mods[i].base; i++) {}
    if (i >= MAX_MODS) return;
    Mod *m = &g_mods[i];
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (!size) size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    ULONG_PTR lo = ~(ULONG_PTR)0, hi = 0;
    for (int k = 0; k < nt->FileHeader.NumberOfSections; k++)
        if (sec[k].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            ULONG_PTR s = (ULONG_PTR)base + sec[k].VirtualAddress;
            ULONG_PTR e = s + (sec[k].Misc.VirtualSize ? sec[k].Misc.VirtualSize : sec[k].SizeOfRawData);
            if (s < lo) lo = s;
            if (e > hi) hi = e;
        }
    m->end = (ULONG_PTR)base + size;
    m->codeLo = hi ? lo : 0;
    m->codeHi = hi;
    const WCHAR *file = path;
    for (int k = 0; k < len; k++) if (path[k] == L'\\' || path[k] == L'/') file = path + k + 1;
    int w = WideCharToMultiByte(CP_UTF8, 0, file, (int)(path + len - file), m->name, sizeof m->name - 1, NULL, NULL);
    m->name[w > 0 ? w : 0] = 0;
    MemoryBarrier();
    m->base = (ULONG_PTR)base;
    if (i == g_modCount) InterlockedIncrement(&g_modCount);
}

static void table_remove(const void *base)
{
    for (LONG i = 0; i < g_modCount; i++)
        if (g_mods[i].base == (ULONG_PTR)base) { g_mods[i].base = 0; return; }
}

static const Mod *table_find(ULONG_PTR a)
{
    LONG n = g_modCount;
    for (LONG i = 0; i < n; i++) {
        ULONG_PTR b = g_mods[i].base;
        if (b && a >= b && a < g_mods[i].end) return &g_mods[i];
    }
    return NULL;
}

const char *module_at(const void *addr, ULONG_PTR *base)
{
    const Mod *m = table_find((ULONG_PTR)addr);
    if (!m) return NULL;
    *base = m->base;
    return m->name;
}

/* ------------------------------------------------------------------ */
/* Events log list                                                     */
/* ------------------------------------------------------------------ */

// Copies a directory into dir with a trailing backslash; 0 if it is unknown.
static int prefix_of(WCHAR *dir, int len)
{
    if (len <= 0 || len >= MAX_PATH - 1) return 0;
    if (dir[len - 1] != L'\\') { dir[len++] = L'\\'; dir[len] = 0; }
    return len;
}

static int starts_with(const WCHAR *path, int len, const WCHAR *dir, int dirLen)
{
    return dirLen && len >= dirLen && _wcsnicmp(path, dir, dirLen) == 0;
}

// path (len characters, not necessarily terminated) as UTF-8, the profile folder abbreviated.
static void display_path(const WCHAR *path, int len, char *out, int cap)
{
    int n = 0;
    if (starts_with(path, len, g_profile, g_profileLen)) {
        n = snprintf(out, cap, "%%USERPROFILE%%\\");
        path += g_profileLen;
        len -= g_profileLen;
    }
    int w = WideCharToMultiByte(CP_UTF8, 0, path, len, out + n, cap - n - 1, NULL, NULL);
    out[n + w] = 0;
}

static void log_module(const char *what, const WCHAR *path, int len, const BYTE *base, DWORD size, int readHeader)
{
    if (starts_with(path, len, g_winDir, g_winDirLen)) return;
    char shown[MAX_PATH * 3];
    display_path(path, len, shown, sizeof shown);
    if (readHeader) {
        const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
        log_event("module %s: base=%p size=0x%lx timestamp=0x%08lx %s", what, (void *)base,
                  (unsigned long)(size ? size : nt->OptionalHeader.SizeOfImage),
                  (unsigned long)nt->FileHeader.TimeDateStamp, shown);
    } else {
        log_event("module %s: base=%p %s", what, (void *)base, shown);
    }
}

// Runs under the loader lock: logging and the table only.
static VOID CALLBACK on_dll(ULONG reason, const DllNotifyData *d, void *ctx)
{
    if (!d || !d->FullDllName || !d->FullDllName->Buffer) return;
    int len = d->FullDllName->Length / sizeof(WCHAR);
    if (reason == DLL_NOTIFY_LOADED) {
        table_add(d->DllBase, d->SizeOfImage, d->FullDllName->Buffer, len);
        log_module("loaded", d->FullDllName->Buffer, len, d->DllBase, d->SizeOfImage, 1);
    } else {
        table_remove(d->DllBase);
        log_module("unloaded", d->FullDllName->Buffer, len, d->DllBase, 0, 0);
    }
}

void modules_init(void)
{
    g_winDirLen = prefix_of(g_winDir, (int)GetWindowsDirectoryW(g_winDir, MAX_PATH - 1));
    g_profileLen = prefix_of(g_profile, (int)GetEnvironmentVariableW(L"USERPROFILE", g_profile, MAX_PATH - 1));

    // Called from DllMain: the loader lock keeps other threads from loading anything between
    // the list and the registration.
    static HMODULE mods[MAX_MODS];
    DWORD bytes = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes)) {
        DWORD n = bytes / sizeof mods[0];
        if (n > MAX_MODS) n = MAX_MODS;
        for (DWORD i = 0; i < n; i++) {
            WCHAR path[MAX_PATH];
            DWORD len = GetModuleFileNameW(mods[i], path, MAX_PATH);
            if (!len) continue;
            table_add((const BYTE *)mods[i], 0, path, (int)len);
            log_module("at start", path, (int)len, (const BYTE *)mods[i], 0, 1);
        }
    }

    LdrRegisterDllNotification_t reg = (LdrRegisterDllNotification_t)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    void *cookie;
    if (!reg || reg(0, on_dll, NULL, &cookie) != 0)
        log_event("module load notifications unavailable; only the startup list is logged");
}

/* ------------------------------------------------------------------ */
/* Stack scan (exception path: no heap, no locks)                      */
/* ------------------------------------------------------------------ */

#define STACK_SCAN_BYTES (64 * 1024)
#define STACK_MAX_FRAMES 24

// p follows a call: E8 rel32, or FF /2 (call r/m32) in any of its 2- to 7-byte encodings.
// The caller has checked that the 7 bytes before p are in the module's code.
static int follows_call(const BYTE *p)
{
    if (p[-5] == 0xE8) return 1;
    for (int k = 2; k <= 7; k++) {
        const BYTE *b = p - k;
        if (b[0] != 0xFF || ((b[1] >> 3) & 7) != 2) continue;
        int mod = b[1] >> 6, rm = b[1] & 7, len = 2;
        if (mod != 3 && rm == 4) { len++; if (mod == 0 && (b[2] & 7) == 5) len += 4; }
        if (mod == 1) len += 1;
        else if (mod == 2) len += 4;
        else if (mod == 0 && rm == 5) len += 4;
        if (len == k) return 1;
    }
    return 0;
}

// a if it is a return address candidate: in a module's code, just after a call.
static const Mod *ret_candidate(ULONG_PTR a)
{
    const Mod *m = table_find(a);
    return m && a >= m->codeLo + 7 && a < m->codeHi && follows_call((const BYTE *)a) ? m : NULL;
}

// Frames for the "stack" lines. A run of the same address is one frame with a count.
typedef struct {
    char line[900];
    int n, frames, repeats;
    ULONG_PTR last;
} StackOut;

static void out_repeats(StackOut *o)
{
    if (o->repeats) o->n += snprintf(o->line + o->n, sizeof o->line - o->n, " (x%d)", o->repeats + 1);
    o->repeats = 0;
}

static void out_add(StackOut *o, const Mod *m, ULONG_PTR a, int count)
{
    if (a == o->last) { o->repeats += count; return; }
    out_repeats(o);
    o->last = a;
    o->repeats = count - 1;
    o->n += snprintf(o->line + o->n, sizeof o->line - o->n, " %s+0x%lx", m->name, (unsigned long)(a - m->base));
    o->frames++;
    if (o->n > (int)sizeof o->line - 80) { out_repeats(o); log_event("stack:%s", o->line); o->n = 0; }
}

// Logs up to 24 frames from the words in [sp, top). For a stack overflow the innermost return
// address is the recursion's: it is logged once with the number of copies on the stack, and
// the scan goes on above its outermost copy, so the recursion's caller shows. Stale values left
// in each recursive frame's locals would otherwise use up the frames first.
static void scan_words(ULONG_PTR sp, ULONG_PTR top, int overflow)
{
    ULONG_PTR end = top - sp > STACK_SCAN_BYTES ? sp + STACK_SCAN_BYTES : top;
    StackOut o = {0};
    if (overflow) {
        const Mod *rm = NULL;
        ULONG_PTR r = 0, p;
        for (p = sp; p < top && !rm; p += 4) rm = ret_candidate(r = *(const ULONG_PTR *)p);
        if (rm) {
            int count = 0;
            for (sp = p - 4; p < top; p += 4)
                if (*(const ULONG_PTR *)p == r) { count++; sp = p; }
            out_add(&o, rm, r, count + 1);
            sp += 4;
            end = top - sp > STACK_SCAN_BYTES ? sp + STACK_SCAN_BYTES : top;
        }
    }
    for (; sp < end && o.frames < STACK_MAX_FRAMES; sp += 4) {
        ULONG_PTR a = *(const ULONG_PTR *)sp;
        const Mod *m = ret_candidate(a);
        if (m) out_add(&o, m, a, 1);
    }
    out_repeats(&o);
    if (o.n) log_event("stack:%s", o.line);
    else if (!o.frames) log_event("stack: no return addresses found");
}

// tib is the faulting thread's, which may not be the calling one.
void modules_log_stack(const CONTEXT *ctx, const NT_TIB *tib, int overflow)
{
    ULONG_PTR sp = ctx->Esp & ~(ULONG_PTR)3, top = (ULONG_PTR)tib->StackBase;
    if (sp < (ULONG_PTR)tib->StackLimit || sp >= top) {
        log_event("stack: esp=%p is outside this thread's stack (%p-%p)", (void *)ctx->Esp, tib->StackLimit, tib->StackBase);
        return;
    }
    scan_words(sp, top, overflow);
}

// A copy of another thread's stack, taken while it was suspended (hang.c).
void modules_log_stack_copy(const void *words, size_t bytes)
{
    scan_words((ULONG_PTR)words, (ULONG_PTR)words + (bytes & ~(size_t)3), 0);
}
