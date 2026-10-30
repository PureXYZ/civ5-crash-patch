// Hardware write watchpoint on one field of a game object ([probe] Watch=1).
//
// The recurring crash calls through the vtable of the FAudioGame singleton (the
// pointer at exe RVA 0x5b54f34, object new'd in FireEngine2\FAudioGame.h) after
// something has overwritten it with 1. To catch the writer, the sampler polls the
// global; when it points at a new object, debug register DR0 is set to trap writes
// to object+WatchOffset (WatchLen bytes) on every thread of the process, and again on
// threads that appear later. Normally nothing writes a vtable after construction,
// so each trap is either the corruption or the object's own destructor.
//
// The trap (EXCEPTION_SINGLE_STEP with DR6.B0) arrives after the writing instruction.
// A first vectored handler logs the thread, the instruction that follows the write,
// the new value and the return addresses on the stack, resolved against a module
// table cached by the sampler (no loader calls inside the handler), then resumes.
//
// WatchGlobalRva (default 0x5b54f34) locates the pointer in the exe; the test exe
// names an exported variable with WatchGlobalExport instead.
//
// WatchMode=dr (above) made the game exit cleanly at the end of startup, 11 s after the
// breakpoints were armed (2026-10-28): Steam's DRM checks the debug registers. So the
// default is WatchMode=page, which uses no debug registers. The exe's malloc and free
// imports are wrapped. The one allocation of WatchSize bytes (0x46C) whose stack holds
// the FAudioGame new call site (exe RVA WatchCallerRva, 0x31942a) gets two pages of
// its own: the object starts 8 bytes before the end of the first page, so only its
// first 8 bytes live there. Once the global points at it, that page goes read-only.
// A write to it faults; the handler logs it (with the stack for writes to the watched
// field), unprotects the page, single-steps the one instruction (trap flag) and
// re-protects. Its free is logged with the stack, then the pages are released.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE *, DWORD, LPDWORD);

#define MAX_ARMED 2048
#define MAX_MODS 256
#define MAX_LOGGED 30

typedef struct { ULONG_PTR base, end; char name[40]; } WMod;

static int g_on;
static void *volatile *g_global;    // the game's pointer to the watched object
static void *g_obj;                 // object currently watched
static ULONG_PTR g_addr;            // watched address (object + offset)
static DWORD g_len = 4, g_offset;
static DWORD g_armed[MAX_ARMED];    // thread ids armed for g_addr
static int g_nArmed;
static WMod g_mods[MAX_MODS];
static volatile LONG g_nMods;
static volatile LONG g_nTraps;
static int g_armFailures;

// Page mode
#define PAGE 4096
typedef void *(__cdecl *wmalloc_t)(size_t);
typedef void(__cdecl *wfree_t)(void *);
static int g_pageMode;
static wmalloc_t o_malloc;
static wfree_t o_free;
static size_t g_isoSize = 0x46C;
static ULONG_PTR g_callerRet;            // return address that marks the allocation, or 0 for any
static BYTE *volatile g_isoBase;         // the two pages
static BYTE *volatile g_isoObj;          // object inside them
static volatile LONG g_isoProtected;
static volatile LONG g_nOtherWrites, g_nSteps, g_nSizeSeen;
static DWORD g_tlsStep = TLS_OUT_OF_INDEXES;  // set while this thread single-steps a write
static int g_handlerAdded;

static const WMod *mod_of(ULONG_PTR a)
{
    LONG n = g_nMods;
    for (LONG i = 0; i < n; i++)
        if (a >= g_mods[i].base && a < g_mods[i].end) return &g_mods[i];
    return NULL;
}

static void refresh_modules(void)
{
    HMODULE mods[MAX_MODS];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
    DWORD n = needed / sizeof(HMODULE);
    if (n > MAX_MODS) n = MAX_MODS;
    if ((LONG)n == g_nMods) return;
    InterlockedExchange(&g_nMods, 0);  // the handler sees an empty table while it is rebuilt
    for (DWORD i = 0; i < n; i++) {
        BYTE *b = (BYTE *)mods[i];
        IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
        WCHAR path[MAX_PATH];
        DWORD len = GetModuleFileNameW(mods[i], path, MAX_PATH);
        const WCHAR *s = path;
        for (DWORD k = 0; k < len; k++)
            if (path[k] == 92) s = path + k + 1;  // 92 = backslash
        WMod *m = &g_mods[i];
        int k = 0;
        for (; s < path + len && k < (int)sizeof m->name - 1; s++) m->name[k++] = *s < 128 && *s != ' ' ? (char)*s : '_';
        m->name[k] = 0;
        m->base = (ULONG_PTR)b;
        m->end = m->base + nt->OptionalHeader.SizeOfImage;
    }
    InterlockedExchange(&g_nMods, (LONG)n);
}

static void fmt_addr(ULONG_PTR a, char *out, int cap)
{
    const WMod *m = mod_of(a);
    if (m) snprintf(out, cap, "%s+0x%lx", m->name, (unsigned long)(a - m->base));
    else snprintf(out, cap, "0x%08lx", (unsigned long)a);
}

// A stack value that points just past a call instruction inside a known module.
static int looks_like_return(ULONG_PTR v)
{
    const WMod *m = mod_of(v);
    if (!m || v - 6 < m->base) return 0;
    BYTE b[6];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(v - 6), b, 6, &got) || got != 6) return 0;
    const BYTE *e = b + 6;
    return e[-5] == 0xE8 || (e[-6] == 0xFF && (e[-5] & 0x38) == 0x10) || (e[-2] == 0xFF && (e[-1] & 0xF8) == 0xD0) ||
           (e[-3] == 0xFF && (e[-2] & 0x38) == 0x10 && (e[-2] & 0xC0) == 0x40) ||
           (e[-2] == 0xFF && (e[-1] & 0x38) == 0x10 && (e[-1] & 0xC0) == 0x00);
}

static LONG CALLBACK on_trap(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    LONG n = InterlockedIncrement(&g_nTraps);
    if (n <= MAX_LOGGED) {
        char line[1400], a[64];
        DWORD now = 0;
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)g_addr, &now, sizeof now, NULL);
        fmt_addr(c->Eip, a, sizeof a);
        int k = snprintf(line, sizeof line, "watch: WRITE #%ld to %p (object %p +0x%lx) by thread %lu after %s: now 0x%08lx; eax=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx; stack:",
                         n, (void *)g_addr, g_obj, (unsigned long)g_offset, GetCurrentThreadId(), a, (unsigned long)now,
                         c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi);
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        const DWORD *p = (const DWORD *)c->Esp, *top = (const DWORD *)tib->StackBase;
        int frames = 0;
        for (int i = 0; p + i < top && i < 1024 && frames < 24 && k < (int)sizeof line - 80; i++) {
            ULONG_PTR v = p[i];
            if (!looks_like_return(v)) continue;
            fmt_addr(v, a, sizeof a);
            k += snprintf(line + k, sizeof line - k, " %s", a);
            frames++;
        }
        log_event("%s", line);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void log_write(CONTEXT *c, ULONG_PTR target, DWORD now, const char *what)
{
    char line[1400], a[64];
    LONG n = InterlockedIncrement(&g_nTraps);
    if (n > MAX_LOGGED) return;
    fmt_addr(c->Eip, a, sizeof a);
    int k = snprintf(line, sizeof line, "watch: %s #%ld to %p (object %p +0x%lx) by thread %lu at %s: now 0x%08lx; eax=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx; stack:",
                     what, n, (void *)target, (void *)g_isoObj, (unsigned long)(target - (ULONG_PTR)g_isoObj),
                     GetCurrentThreadId(), a, (unsigned long)now, c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp);
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    const DWORD *p = (const DWORD *)c->Esp, *top = (const DWORD *)tib->StackBase;
    int frames = 0;
    for (int i = 0; p + i < top && i < 1024 && frames < 24 && k < (int)sizeof line - 80; i++) {
        if (!looks_like_return(p[i])) continue;
        fmt_addr(p[i], a, sizeof a);
        k += snprintf(line + k, sizeof line - k, " %s", a);
        frames++;
    }
    log_event("%s", line);
}

// Write fault on the protected page: let the instruction through one step at a time.
static LONG CALLBACK on_page_fault(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    BYTE *base = g_isoBase;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && base && er->NumberParameters >= 2 &&
        er->ExceptionInformation[0] == 1 && er->ExceptionInformation[1] >= (ULONG_PTR)base &&
        er->ExceptionInformation[1] < (ULONG_PTR)base + PAGE) {
        TlsSetValue(g_tlsStep, (void *)(er->ExceptionInformation[1] | 1));  // remember the target (odd = pending)
        DWORD old;
        VirtualProtect(base, PAGE, PAGE_READWRITE, &old);
        c->EFlags |= 0x100;  // trap after the write
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && g_tlsStep != TLS_OUT_OF_INDEXES) {
        ULONG_PTR pending = (ULONG_PTR)TlsGetValue(g_tlsStep);
        if (!(pending & 1)) return EXCEPTION_CONTINUE_SEARCH;
        TlsSetValue(g_tlsStep, NULL);
        c->EFlags &= ~0x100u;
        InterlockedIncrement(&g_nSteps);
        ULONG_PTR target = pending & ~(ULONG_PTR)1;
        BYTE *obj = g_isoObj;
        if (obj && target >= (ULONG_PTR)obj + g_offset && target < (ULONG_PTR)obj + g_offset + g_len)
            log_write(c, target, *(volatile DWORD *)((ULONG_PTR)obj + g_offset), "WRITE");
        else
            InterlockedIncrement(&g_nOtherWrites);
        if (g_isoProtected && g_isoBase) {
            DWORD old;
            VirtualProtect(g_isoBase, PAGE, PAGE_READONLY, &old);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int stack_has(const void *from, ULONG_PTR ret)
{
    const DWORD *p = from;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    for (int i = 0; i < 64 && (const BYTE *)(p + i) < (const BYTE *)tib->StackBase; i++)
        if (p[i] == ret) return 1;
    return 0;
}

// Logs the first allocations of the watched size with the caller check's result, so a
// miss can be told apart from an allocation that never comes through here.
static void log_size_seen(const void *from, int match)
{
    LONG k = InterlockedIncrement(&g_nSizeSeen);
    if (k > 12) return;
    char line[700], a[64];
    int n = snprintf(line, sizeof line, "watch: %lu-byte malloc #%ld on thread %lu, caller %s, isolation %s; stack:",
                     (unsigned long)g_isoSize, k, GetCurrentThreadId(), match ? "MATCHES" : "differs",
                     g_isoBase ? "already used" : "free");
    const DWORD *p = from;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    int frames = 0;
    for (int i = 0; i < 64 && (const BYTE *)(p + i) < (const BYTE *)tib->StackBase && frames < 8; i++) {
        if (!looks_like_return(p[i])) continue;
        fmt_addr(p[i], a, sizeof a);
        n += snprintf(line + n, sizeof line - n, " %s", a);
        frames++;
    }
    log_event("%s", line);
}

static void *__cdecl w_malloc(size_t n)
{
    int match = 0;
    if (n == g_isoSize) {
        // The caller's frames start at our first argument. Not &n: reading past a single
        // variable is undefined, and clang then compared only n itself (found 2026-10-28).
        const DWORD *args = (const DWORD *)__builtin_frame_address(0) + 2;
        match = !g_callerRet || stack_has(args, g_callerRet);
        log_size_seen(args, match);
    }
    if (match && !g_isoBase) {
        BYTE *b = VirtualAlloc(NULL, 2 * PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (b && InterlockedCompareExchangePointer((void *volatile *)&g_isoBase, b, NULL) == NULL) {
            g_isoObj = b + PAGE - 8;  // only the first 8 bytes on the first page
            log_event("watch: isolated it at %p", (void *)g_isoObj);
            return g_isoObj;
        }
        if (b) VirtualFree(b, 0, MEM_RELEASE);
    }
    return o_malloc(n);
}

static void __cdecl w_free(void *p)
{
    BYTE *base = g_isoBase;
    if (p && base && p == g_isoObj) {
        CONTEXT c;
        RtlCaptureContext(&c);
        c.Eip = (DWORD)(ULONG_PTR)__builtin_return_address(0);
        InterlockedExchange(&g_isoProtected, 0);
        DWORD old;
        VirtualProtect(base, PAGE, PAGE_READWRITE, &old);
        log_write(&c, (ULONG_PTR)p, *(DWORD *)p, "FREE of the watched object");
        g_isoObj = NULL;
        g_isoBase = NULL;
        VirtualFree(base, 0, MEM_RELEASE);
        return;
    }
    o_free(p);
}

// Wraps the exe's malloc and free imports (matched by name, whichever DLL they come from).
static int patch_exe_alloc(BYTE *exe)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(exe + dir.VirtualAddress); dir.Size && d->Name; d++) {
        if (!d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA32 *names = (IMAGE_THUNK_DATA32 *)(exe + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA32 *slots = (IMAGE_THUNK_DATA32 *)(exe + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char *name = (const char *)((IMAGE_IMPORT_BY_NAME *)(exe + names->u1.AddressOfData))->Name;
            void *to = NULL;
            if (!strcmp(name, "malloc") && !o_malloc) { o_malloc = (wmalloc_t)(ULONG_PTR)slots->u1.Function; to = (void *)w_malloc; }
            else if (!strcmp(name, "free") && !o_free) { o_free = (wfree_t)(ULONG_PTR)slots->u1.Function; to = (void *)w_free; }
            if (!to) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, 4, PAGE_READWRITE, &old)) continue;
            InterlockedExchange((volatile LONG *)&slots->u1.Function, (LONG)(ULONG_PTR)to);
            VirtualProtect(&slots->u1.Function, 4, old, &old);
            n++;
        }
    }
    return n;
}

static int is_armed(DWORD tid)
{
    for (int i = 0; i < g_nArmed; i++)
        if (g_armed[i] == tid) return 1;
    return 0;
}

// Sets (addr != 0) or clears DR0 on one thread. Nothing that can take a lock runs
// while the thread is suspended.
static int set_dr0(DWORD tid, ULONG_PTR addr)
{
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!t) return 0;
    int ok = 0;
    if (SuspendThread(t) != (DWORD)-1) {
        CONTEXT c;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            DWORD lenBits = g_len == 1 ? 0 : g_len == 2 ? 1 : 3;
            c.Dr7 &= ~(DWORD)(0x3 | (0xF << 16));  // DR0: enable bits, R/W and LEN
            c.Dr0 = addr;
            if (addr) c.Dr7 |= 1 | (1 << 16) | (lenBits << 18);  // local enable, break on writes
            c.Dr6 = 0;
            ok = SetThreadContext(t, &c) != 0;
        }
        ResumeThread(t);
    }
    CloseHandle(t);
    return ok;
}

// Arms every thread of the process that isn't armed for the current address yet.
static void arm_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    int added = 0, failed = 0;
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || is_armed(te.th32ThreadID)) continue;
        if (g_nArmed >= MAX_ARMED) break;
        if (set_dr0(te.th32ThreadID, g_addr)) { g_armed[g_nArmed++] = te.th32ThreadID; added++; }
        else failed++;
    }
    CloseHandle(snap);
    if (failed && g_armFailures < 5) { g_armFailures++; log_event("watch: could not arm %d thread(s)", failed); }
    if (added > 3) log_event("watch: armed %d threads (%d in total)", added, g_nArmed);
}

static void disarm_all(void)
{
    for (int i = 0; i < g_nArmed; i++) set_dr0(g_armed[i], 0);
    g_nArmed = 0;
}

void watch_tick(void)
{
    if (!g_on) return;
    refresh_modules();
    if (g_pageMode) {
        if (!g_handlerAdded) { AddVectoredExceptionHandler(1, on_page_fault); g_handlerAdded = 1; }  // ahead of the probe's own
        void *obj = *g_global;
        if (obj != g_obj) {
            log_event("watch: object pointer is now %p (was %p)%s", obj, g_obj,
                      obj && obj == g_isoObj ? "; it is the isolated allocation, protecting its first page" : "");
            g_obj = obj;
        }
        if (obj && obj == g_isoObj && !g_isoProtected) {
            DWORD old;
            InterlockedExchange(&g_isoProtected, 1);
            VirtualProtect(g_isoBase, PAGE, PAGE_READONLY, &old);
        }
        return;
    }
    void *obj = *g_global;
    if (obj != g_obj) {
        if (g_obj) disarm_all();
        char vt[64] = "?";
        DWORD v = 0;
        if (obj && ReadProcessMemory(GetCurrentProcess(), obj, &v, sizeof v, NULL)) fmt_addr(v, vt, sizeof vt);
        log_event("watch: object pointer is now %p (was %p); vtable %s", obj, g_obj, vt);
        g_obj = obj;
        g_addr = obj ? (ULONG_PTR)obj + g_offset : 0;
    }
    if (g_addr) arm_threads();
}

void watch_init(const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"Watch", 0, ini)) return;
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    WCHAR wname[64];
    GetPrivateProfileStringW(L"probe", L"WatchGlobalExport", L"", wname, 64, ini);
    if (wname[0]) {
        char name[64];
        WideCharToMultiByte(CP_ACP, 0, wname, -1, name, sizeof name, NULL, NULL);
        g_global = (void *volatile *)GetProcAddress((HMODULE)exe, name);
    } else {
        WCHAR rva[32];
        GetPrivateProfileStringW(L"probe", L"WatchGlobalRva", L"0x5b54f34", rva, 32, ini);
        g_global = (void *volatile *)(exe + wcstoul(rva, NULL, 0));
    }
    g_offset = GetPrivateProfileIntW(L"probe", L"WatchOffset", 0, ini);
    g_len = GetPrivateProfileIntW(L"probe", L"WatchLen", 4, ini);
    if (g_len != 1 && g_len != 2) g_len = 4;
    g_offset &= ~(g_len - 1);  // the hardware needs the address aligned to the length
    if (!g_global) { log_event("watch: global not found; off"); return; }
    WCHAR mode[16];
    GetPrivateProfileStringW(L"probe", L"WatchMode", L"page", mode, 16, ini);
    g_pageMode = _wcsicmp(mode, L"dr") != 0;
    if (g_pageMode) {
        WCHAR v[32];
        GetPrivateProfileStringW(L"probe", L"WatchSize", L"0x46C", v, 32, ini);
        g_isoSize = wcstoul(v, NULL, 0);
        GetPrivateProfileStringW(L"probe", L"WatchCallerRva", wname[0] ? L"0" : L"0x31942a", v, 32, ini);
        ULONG_PTR rva = wcstoul(v, NULL, 0);
        g_callerRet = rva ? (ULONG_PTR)exe + rva : 0;
        g_tlsStep = TlsAlloc();
        refresh_modules();  // early allocations need it for their stacks
        int n = g_tlsStep != TLS_OUT_OF_INDEXES ? patch_exe_alloc(exe) : 0;
        if (n != 2 || !o_malloc || !o_free) { log_event("watch: exe malloc/free imports not found (%d); off", n); return; }
        g_on = 1;
        log_event("watch: ON (page mode), isolating the %lu-byte allocation%s; watching writes to object+0x%lx (%lu bytes)",
                  (unsigned long)g_isoSize, g_callerRet ? " from the FAudioGame call site" : "", (unsigned long)g_offset,
                  (unsigned long)g_len);
        return;
    }
    AddVectoredExceptionHandler(1, on_trap);
    g_on = 1;
    log_event("watch: ON, watching writes to [%p]+0x%lx (%lu bytes)", (void *)g_global, (unsigned long)g_offset,
              (unsigned long)g_len);
}

void watch_report(void)
{
    if (!g_on) return;
    if (g_pageMode)
        log_event("watch: %ld logged events, %ld other writes to the protected page, %ld single steps; object %s",
                  g_nTraps, g_nOtherWrites, g_nSteps, g_isoObj ? "isolated" : "not isolated (yet)");
    else
        log_event("watch: %ld write traps in total, %d threads armed", g_nTraps, g_nArmed);
}