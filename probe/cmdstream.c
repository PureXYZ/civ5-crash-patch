// Render command stream overruns (core\CommandStream.cpp) on big maps.
//
// A command stream (exe+0x4ce150 constructor, args a1, a2, bytes, 0) is double-buffered and
// holds three arrays, each split into two halves that swap at the frame flip (exe+0x4cdd80):
//   commands: a1 entries of 0x20 bytes per half, [s+0x14] / [s+0x2070], count [s+0xc]
//             (malloc'd, returning to exe+0x4ce19a). Entry: +0 key, +4 -1 or arg, +8 word,
//             +0x10 arg, +0x14 record pointer, +0x18 record size.
//   groups:   a2 entries of 0x80 bytes per half, [s] / [s+0x206c], count [s+4]
//             (malloc'd, returning to exe+0x4ce1c5); +0x5c = index of the group's first command.
//   data:     records, round16(bytes) per half, [s+0x24] / [s+0x2068], size [s+0x34], offset
//             [s+0x40] (aligned new, returning to exe+0x4ce1e5).
// Plus a 1024-dword list [s+0x2078], count [s+0x207c] (exe+0x4cce90 only).
// None of the writers checks a capacity: the record allocator exe+0x4cfdc0 is a bare bump
// allocator, the three command appenders (exe+0x4ccd80 / +0x4cce00 / +0x4cce90) and five group
// writers (exe+0x4cd930..+0x4cdd20) write at index = count and count++.
//
// What went wrong on the Huge-map save (2026-10-28): the stream created at exe+0x1c9964 (the
// single-buffered branch; exe+0x1c993b / +0x1c995d create a pair otherwise) has a1 = 0x1000,
// a2 = 0x400 and 1 MB per half. Busy frames (~1700 units, scrolling) overflow all of it:
//   - data past 1 MB: crashes writing past the buffer (exe+0x20932e, exe+0x1e0e63, exe+0x1062c0);
//   - commands past 4096: 0x20-byte entries written past the 256 KB command array into whatever
//     heap block follows. One of those is FAudioSystemMiles's sample table ({flag, HSAMPLE, flag}
//     x n, exe+0x521820): its handles became 0xFFFFFFFF (entry +4) or a pointer into the data
//     buffer (entry +0x14: HSAMPLE 0x2F1E4F10 in the stream's own 4 MB data block, session
//     20261028_235227), and Miles crashed on them (AIL_sample_status, AIL_stop_sample,
//     AIL_set_sample_volume_pan). Before any of these fixes the same overflow corrupted the
//     FAudioGame object (vtable = 1) - the September crash family.
//
// Fixes here:
//  1. Slack (CommandStreamSlackKB, default 4): every constructor allocation gets that much room
//     past its end (the malloc / _aligned_malloc import wrappers see the constructor's return
//     addresses on the stack).
//  2. Spare halves (CommandStreamSplit=1) for the streams that overflow, identified by the
//     constructor's caller (CommandStreamSpareCallers, default the exe+0x1c99xx sites above):
//     each half of each array gets a spare area of its own, laid out [half 1][spare][half 2][spare].
//     The game's sizes (+0x8, +0x10, +0x34) are unchanged and entries keep their half-relative
//     addresses, so overflow is consumed exactly as the unmodified game consumes it, only in
//     memory that belongs to the stream. Commands get CommandStreamCmdSpare x a1 spare entries
//     (default 3), groups CommandStreamGroupSpare x a2 (default 1), data min(half, 1 MB). The
//     wrappers enlarge the allocations and note them ("grants"); the constructor stubs at
//     exe+0x4ce1e8 (data) and exe+0x4ce20a (commands, groups) place the second halves from the
//     grants, so the layout always matches what was allocated.
//  3. Bounds (CommandStreamCap=1): the record allocator is replaced (records past half + spare
//     come from a per-stream overflow pool and are left out of the frame), and the command,
//     group and list counters stop at their half's limit (see "Bounded counters"), so later
//     entries overwrite one slot instead of running on through memory.
//  4. Stats (CommandStreamStats=1): the frame flip call (exe+0x4bd9a7) goes through flip_note,
//     which keeps each stream's peak use, logs the first frame that goes past the game's own
//     capacity or fills a half, and cmdstream_report lists them.
//
// Other builds (exes.c): the tablet exe is laid out as the DX11 one, but its record allocator
// rounds each record up to 256 bytes. The DirectX 9 exe's streams have one set of arrays (no
// halves, groups of 0x90 bytes), reset by a function of their own each frame (exe+0x4eca40), so:
// the spare is simply more room at the end of each array, every constructor allocation is noted
// with its spare (newest first: a live stream's arrays come from its constructor), the counters
// and the record allocator stop at what was allocated (looked up only once a count reaches the
// game's own capacity), and the stats come from the reset.
//
// Rejected: CommandStreamMB (rewrite the pair's `push 100000h` at exe+0x1c992a / +0x1c9942).
// With 4 MB the terrain stopped rendering and the game hit its error path 116 times and
// exited (session 20261028_231026): something downstream depends on the half size field.
// Kept as an option, default 0. A spare area for every stream (the 2026-10-29 build) cost
// ~34 MB of address space; it is now only for the streams that need it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <wchar.h>
#include "probe.h"

typedef void *(__cdecl *malloc_t)(size_t);
typedef void *(__cdecl *amalloc_t)(size_t, size_t);

static const BYTE kCtorStart[] = {0x8b, 0x44, 0x24, 0x04, 0x53, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x18};  // exe+0x4ce150

static BYTE *g_exe;
static const GameExe *g_g;
static int g_halves = 2, g_round;  // the stream layout (2 halves or 1) and the tablet's record rounding
static malloc_t o_malloc;
static amalloc_t o_amalloc;
static ULONG_PTR g_rets[3];
static size_t g_slack;
static volatile LONG g_nPadded;

static const BYTE kBump[37] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x48, 0x40, 0x8b, 0x50, 0x24, 0x03, 0xd1, 0x89,
                               0x50, 0x30, 0x8b, 0x54, 0x24, 0x08, 0x56, 0x0f, 0xb7, 0xf2, 0x03, 0xf1, 0x89,
                               0x70, 0x40, 0x66, 0x89, 0x50, 0x4c, 0x8b, 0x40, 0x30, 0x5e, 0xc2};
// The tablet's: the same with `add ecx, 0FFh / and ecx, 0FF00h` on the size.
static const BYTE kBumpRound[51] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x50, 0x40, 0x8b, 0x48, 0x24, 0x03, 0xca, 0x89,
                                    0x48, 0x30, 0x8b, 0x4c, 0x24, 0x08, 0x81, 0xc1, 0xff, 0x00, 0x00, 0x00, 0x56,
                                    0x81, 0xe1, 0x00, 0xff, 0x00, 0x00, 0x0f, 0xb7, 0xf1, 0x03, 0xf2, 0x89, 0x70,
                                    0x40, 0x66, 0x89, 0x48, 0x4c, 0x8b, 0x40, 0x30, 0x5e, 0xc2, 0x08, 0x00};
static volatile LONG g_nDropped, g_nScratch;
static int g_capOn, g_splitOn, g_boundOn, g_statsOn;

// ---- Spare policy and grants ----------------------------------------------------------------

#define SPLIT_MIN 0x1000u
#define SPLIT_MAX 0x100000u
#define MAX_SPARE_CALLERS 8
static ULONG_PTR g_spareCallers[MAX_SPARE_CALLERS];
static int g_nSpareCallers;
static DWORD g_cmdSpare = 3, g_groupSpare = 1;

// The data spare area that follows a half of `half` bytes.
static DWORD spare_for(DWORD half) { return half < SPLIT_MIN ? 0 : half > SPLIT_MAX ? SPLIT_MAX : half; }

static int spared(ULONG_PTR ctorRet)
{
    for (int i = 0; i < g_nSpareCallers; i++)
        if (g_spareCallers[i] == ctorRet) return 1;
    return 0;
}

// An allocation the wrappers enlarged: `spare` extra bytes per half. The constructor stubs take
// the grant for the pointer they were given, so the halves are laid out from what was
// actually allocated (no grant: the game's layout).
#define MAX_GRANTS 32
typedef struct { void *p; DWORD spare; } Grant;
static Grant g_grants[MAX_GRANTS];
static int g_nextGrant;
static volatile LONG g_grantLock;

static void grant_add(void *p, DWORD spare)
{
    while (InterlockedExchange(&g_grantLock, 1)) YieldProcessor();
    g_grants[g_nextGrant] = (Grant){p, spare};
    g_nextGrant = (g_nextGrant + 1) % MAX_GRANTS;
    InterlockedExchange(&g_grantLock, 0);
}

static DWORD grant_take(void *p)
{
    DWORD spare = 0;
    if (!p) return 0;
    while (InterlockedExchange(&g_grantLock, 1)) YieldProcessor();
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].p == p) { spare = g_grants[i].spare; g_grants[i].p = NULL; break; }
    InterlockedExchange(&g_grantLock, 0);
    return spare;
}

__declspec(dllexport) void CivProbe_TestGrant(void *p, DWORD spare) { grant_add(p, spare); }  // for the test

// One set of arrays (DX9): every constructor allocation with its spare bytes (0 for most) and the
// constructor's return address, newest first. A live stream's arrays come from its constructor,
// so the newest note for an array's address is its own; an older note that has been overwritten
// only means no spare (the game's capacity).
#define MAX_NOTES 512
typedef struct { void *p; DWORD spare; ULONG_PTR ctorRet; } Note;
static Note g_notes[MAX_NOTES];
static LONG g_nNotes;

static void note_add(void *p, DWORD spare, ULONG_PTR ctorRet)
{
    while (InterlockedExchange(&g_grantLock, 1)) YieldProcessor();
    g_notes[g_nNotes % MAX_NOTES] = (Note){p, spare, ctorRet};
    g_nNotes++;
    InterlockedExchange(&g_grantLock, 0);
}

static DWORD note_spare(const void *p, ULONG_PTR *ctorRet)
{
    DWORD spare = 0;
    if (ctorRet) *ctorRet = 0;
    if (!p) return 0;
    while (InterlockedExchange(&g_grantLock, 1)) YieldProcessor();
    LONG n = g_nNotes < MAX_NOTES ? g_nNotes : MAX_NOTES;
    for (LONG i = 1; i <= n; i++) {
        const Note *t = &g_notes[(g_nNotes - i) % MAX_NOTES];
        if (t->p != p) continue;
        spare = t->spare;
        if (ctorRet) *ctorRet = t->ctorRet;
        break;
    }
    InterlockedExchange(&g_grantLock, 0);
    return spare;
}

__declspec(dllexport) void CivProbe_TestNote(void *p, DWORD spare) { note_add(p, spare, 0); }  // for the test
// ---- Per-stream stats -----------------------------------------------------------------------

#define MAX_STREAMS 96
typedef struct {
    BYTE *s;
    DWORD creator;                         // RVA of the constructor's return address
    DWORD cmds, groups, data;              // the game's capacities per half
    DWORD cmdSpare, groupSpare, dataSpare; // spare per half (entries, entries, bytes)
    DWORD maxCmds, maxGroups, maxData, maxList;
    DWORD frames, overFrames, fullFrames;
    BYTE warnedOver, warnedFull;
} Stream;
static Stream g_streams[MAX_STREAMS];
static volatile LONG g_nStreams;
static volatile LONG g_streamLock;

static Stream *stream_find(BYTE *s)
{
    LONG n = g_nStreams;
    for (LONG i = 0; i < n; i++)
        if (g_streams[i].s == s) return &g_streams[i];
    return NULL;
}

static void stream_register(BYTE *s, ULONG_PTR ctorRet, DWORD a1, DWORD a2, DWORD cmdSpare, DWORD groupSpare, DWORD dataSpare)
{
    while (InterlockedExchange(&g_streamLock, 1)) YieldProcessor();
    Stream *t = stream_find(s);
    int fresh = !t && g_nStreams < MAX_STREAMS;
    if (fresh) t = &g_streams[g_nStreams];
    if (t) {
        Stream v = {0};
        v.s = s;
        v.creator = (DWORD)(ctorRet - (ULONG_PTR)g_exe);
        v.cmds = a1; v.groups = a2; v.data = *(DWORD *)(s + 0x34);
        v.cmdSpare = cmdSpare; v.groupSpare = groupSpare; v.dataSpare = dataSpare;
        *t = v;
        if (fresh) InterlockedIncrement(&g_nStreams);
    }
    InterlockedExchange(&g_streamLock, 0);
}


// One set of arrays: a stream seen for the first time at its reset, from its constructor's notes.
static void stream_register1(BYTE *s)
{
    ULONG_PTR ret;
    DWORD cmdSpare = note_spare(*(BYTE **)(s + 0x14), &ret);
    if (!ret) return;  // not from the constructor (or its note is gone)
    stream_register(s, ret, *(DWORD *)(s + 8), *(DWORD *)(s + 0x10), cmdSpare / 0x20, note_spare(*(BYTE **)s, NULL) / 0x90,
                    note_spare(*(BYTE **)(s + 0x24), NULL));
}

// ---- Constructor stubs ----------------------------------------------------------------------

static const BYTE kSplit[11] = {0x89, 0x46, 0x24, 0x03, 0xc7, 0x89, 0x86, 0x68, 0x20, 0x00, 0x00};
static const BYTE kHalves[11] = {0xc1, 0xe1, 0x07, 0x03, 0x0e, 0xc1, 0xe2, 0x05, 0x03, 0x56, 0x14};

// Data halves: [s+0x24] = buffer, [s+0x2068] = buffer + half + spare (the grant for buffer).
BYTE *__cdecl split_decide(BYTE *s, BYTE *buf, DWORD half)
{
    DWORD spare = grant_take(buf);
    *(BYTE **)(s + 0x24) = buf;
    *(BYTE **)(s + 0x2068) = buf + half + spare;
    return buf + half;
}

// Command and group halves. Returns the second command half in the high dword and the second
// group half in the low dword; registers the stream for the stats.
unsigned long long __cdecl halves_decide(BYTE *s, DWORD a2, DWORD a1, ULONG_PTR ctorRet)
{
    BYTE *cmds = *(BYTE **)(s + 0x14), *groups = *(BYTE **)s;
    DWORD cmdSpare = grant_take(cmds), groupSpare = grant_take(groups);
    BYTE *cmd2 = cmds + a1 * 0x20 + cmdSpare, *group2 = groups + a2 * 0x80 + groupSpare;
    BYTE *d1 = *(BYTE **)(s + 0x24), *d2 = *(BYTE **)(s + 0x2068);
    DWORD half = *(DWORD *)(s + 0x34), dist = (DWORD)(d2 > d1 ? d2 - d1 : d1 - d2);
    stream_register(s, ctorRet, a1, a2, cmdSpare / 0x20, groupSpare / 0x80, dist > half ? dist - half : 0);
    return ((unsigned long long)(ULONG_PTR)cmd2 << 32) | (ULONG_PTR)group2;
}

// exe+0x4ce1e8 (`mov [esi+24h],eax / add eax,edi / mov [esi+2068h],eax`) calls this with
// esi = stream, eax = data buffer, edi = half size. eax, ecx, edx are free there.
void split_stub(void);
__asm__(".text\n.globl _split_stub\n_split_stub:\n"
        "\tpushl %edi\n"
        "\tpushl %eax\n"
        "\tpushl %esi\n"
        "\tcall _split_decide\n"
        "\taddl $12, %esp\n"
        "\tret\n");

// exe+0x4ce20a (`shl ecx,7 / add ecx,[esi] / shl edx,5 / add edx,[esi+14h]`) calls this with
// esi = stream, ecx = a2, edx = a1; it must leave ecx = second group half, edx = second
// command half. The constructor's own return address is at [esp+0x1c] (above it: the 3 arguments
// pushed at exe+0x4ce204 for the next allocation, and edi, esi, ebx).
void halves_stub(void);
__asm__(".text\n.globl _halves_stub\n_halves_stub:\n"
        "\tpushl 0x1c(%esp)\n"
        "\tpushl %edx\n"
        "\tpushl %ecx\n"
        "\tpushl %esi\n"
        "\tcall _halves_decide\n"
        "\taddl $16, %esp\n"
        "\tmovl %eax, %ecx\n"
        "\tret\n");

__declspec(dllexport) void *CivProbe_SplitStub(void) { return (void *)split_stub; }    // for the test
__declspec(dllexport) void *CivProbe_HalvesStub(void) { return (void *)halves_stub; }  // for the test

// ---- Bounded counters -----------------------------------------------------------------------
// A counter stops at its half's limit, so later entries keep overwriting the slot at the limit
// instead of marching on through memory. The slots per half come from the distance between the
// two half pointers. With a spare area (more slots than the game's own capacity, [s+8] for
// commands, [s+0x10] for groups) the limit is the last spare slot, so even that write stays
// inside the half. Without one the limit is the game's own capacity: the stream keeps every
// slot it had (some have exactly one), and only the entries past it - which the unmodified
// game also writes out of bounds, without end - share the one slot after the half (the other
// half's first slot, or the 4 KB slack). A zero distance (no array) leaves the counter alone.
// The list has 1024 slots and stops at 1023.
// One set of arrays (DX9): the slots are what the constructor allocated (its note), and the same
// rule applies; below the game's own capacity the stubs don't look it up.

volatile LONG g_nCmdDrop, g_nGroupDrop, g_nListDrop;

// The last index a counter may reach, for one set of arrays (0xFFFFFFFF: no array, leave it alone).
static DWORD limit1(const BYTE *array, DWORD gameCap, DWORD entry)
{
    if (!array) return 0xFFFFFFFF;
    DWORD slots = gameCap + note_spare(array, NULL) / entry;
    return slots > gameCap ? slots - 1 : gameCap;
}
DWORD __cdecl cmd_limit1(BYTE *s) { return limit1(*(BYTE **)(s + 0x14), *(DWORD *)(s + 8), 0x20); }
DWORD __cdecl group_limit1(BYTE *s) { return limit1(*(BYTE **)s, *(DWORD *)(s + 0x10), 0x90); }

void cmd_inc(void), cmd_ret0c(void), cmd_ret10(void), cmd_list(void), cmd_inc1(void), cmd_ret08di(void);
void group_inc(void), group_end(void), group_esi28(void), group_ret08(void), group_ret0c(void), group_inc1(void);
void *g_cmdInc = (void *)cmd_inc, *g_groupInc = (void *)group_inc;  // the layout's counter step
__asm__(".text\n"
        // ecx = stream: bounded ++[ecx+0xc]. Keeps eax, ecx; uses edx.
        ".globl _cmd_inc\n_cmd_inc:\n"
        "\tmovl 0x2070(%ecx), %edx\n"
        "\tsubl 0x14(%ecx), %edx\n"
        "\tjns 1f\n"
        "\tnegl %edx\n"
        "1:\tshrl $5, %edx\n"
        "\tjz 2f\n"
        "\tcmpl 8(%ecx), %edx\n"
        "\tjbe 3f\n"
        "\tdecl %edx\n"
        "3:\tcmpl %edx, 0xc(%ecx)\n"
        "\tjb 2f\n"
        "\tlock incl _g_nCmdDrop\n"
        "\tret\n"
        "2:\tincl 0xc(%ecx)\n"
        "\tret\n"
        // One set of arrays: the same with the limit from cmd_limit1 once the count reaches the
        // game's capacity [ecx+8]. Keeps eax, ecx, edx.
        ".globl _cmd_inc1\n_cmd_inc1:\n"
        "\tpushl %edx\n"
        "\tmovl 0xc(%ecx), %edx\n"
        "\tcmpl 8(%ecx), %edx\n"
        "\tjb 2f\n"
        "\tpushl %eax\n"
        "\tpushl %ecx\n"
        "\tpushl %ecx\n"
        "\tcall _cmd_limit1\n"
        "\taddl $4, %esp\n"
        "\tpopl %ecx\n"
        "\tmovl %eax, %edx\n"
        "\tpopl %eax\n"
        "\tcmpl 0xc(%ecx), %edx\n"
        "\tja 2f\n"
        "\tlock incl _g_nCmdDrop\n"
        "\tpopl %edx\n"
        "\tret\n"
        "2:\tincl 0xc(%ecx)\n"
        "\tpopl %edx\n"
        "\tret\n"
        // exe+0x4ccdf9 / +0x4cce7b: `inc [ecx+0Ch] / pop esi / ret 0Ch|10h`
        ".globl _cmd_ret0c\n_cmd_ret0c:\n"
        "\tcall *_g_cmdInc\n"
        "\tpopl %esi\n"
        "\tret $0xc\n"
        ".globl _cmd_ret10\n_cmd_ret10:\n"
        "\tcall *_g_cmdInc\n"
        "\tpopl %esi\n"
        "\tret $0x10\n"
        // exe+0x4ccefd: the command count, then [s+0x2078][[s+0x207c]++] = eax (1024 slots),
        // then pop edi / pop esi / pop ebx / ret 8 with edx = 1 as the original leaves it.
        ".globl _cmd_list\n_cmd_list:\n"
        "\tcall _cmd_inc\n"
        "\tmovl 0x207c(%ecx), %esi\n"
        "\tmovl 0x2078(%ecx), %edi\n"
        "\tcmpl $0x3ff, %esi\n"
        "\tjb 3f\n"
        "\tmovl $0x3ff, %esi\n"
        "\tmovl %eax, (%edi,%esi,4)\n"
        "\tlock incl _g_nListDrop\n"
        "\tjmp 4f\n"
        "3:\tmovl %eax, (%edi,%esi,4)\n"
        "\tincl 0x207c(%ecx)\n"
        "4:\tmovl $1, %edx\n"
        "\tpopl %edi\n"
        "\tpopl %esi\n"
        "\tpopl %ebx\n"
        "\tret $8\n"
        // DX9 exe+0x4ec38c: `inc [ecx+0Ch] / pop edi / pop esi / ret 8` (no list)
        ".globl _cmd_ret08di\n_cmd_ret08di:\n"
        "\tcall *_g_cmdInc\n"
        "\tpopl %edi\n"
        "\tpopl %esi\n"
        "\tret $8\n"
        // ecx = stream: eax = [ecx+4] (the old count, as the game leaves it), bounded ++[ecx+4].
        ".globl _group_inc\n_group_inc:\n"
        "\tmovl 4(%ecx), %eax\n"
        "\tmovl 0x206c(%ecx), %edx\n"
        "\tsubl (%ecx), %edx\n"
        "\tjns 1f\n"
        "\tnegl %edx\n"
        "1:\tshrl $7, %edx\n"
        "\tjz 2f\n"
        "\tcmpl 0x10(%ecx), %edx\n"
        "\tjbe 3f\n"
        "\tdecl %edx\n"
        "3:\tcmpl %edx, %eax\n"
        "\tjb 2f\n"
        "\tlock incl _g_nGroupDrop\n"
        "\tret\n"
        "2:\tleal 1(%eax), %edx\n"
        "\tmovl %edx, 4(%ecx)\n"
        "\tret\n"
        // One set of arrays: the same with the limit from group_limit1 once the count reaches the
        // game's capacity [ecx+0x10].
        ".globl _group_inc1\n_group_inc1:\n"
        "\tmovl 4(%ecx), %eax\n"
        "\tcmpl 0x10(%ecx), %eax\n"
        "\tjb 2f\n"
        "\tpushl %ecx\n"
        "\tpushl %ecx\n"
        "\tcall _group_limit1\n"
        "\taddl $4, %esp\n"
        "\tpopl %ecx\n"
        "\tmovl %eax, %edx\n"
        "\tmovl 4(%ecx), %eax\n"
        "\tcmpl %edx, %eax\n"
        "\tjb 2f\n"
        "\tlock incl _g_nGroupDrop\n"
        "\tret\n"
        "2:\tleal 1(%eax), %edx\n"
        "\tmovl %edx, 4(%ecx)\n"
        "\tret\n"
        // exe+0x4cdb30: `mov byte [ecx+18h],0` + the increment, pop esi / ret
        ".globl _group_end\n_group_end:\n"
        "\tmovb $0, 0x18(%ecx)\n"
        "\tcall *_g_groupInc\n"
        "\tpopl %esi\n"
        "\tret\n"
        // exe+0x4cdc36: stream in esi; pop esi / ret 28h
        ".globl _group_esi28\n_group_esi28:\n"
        "\tmovl %esi, %ecx\n"
        "\tcall *_g_groupInc\n"
        "\tleal 1(%eax), %ecx\n"
        "\tpopl %esi\n"
        "\tret $0x28\n"
        // exe+0x4cdc9e / +0x4cdd6e (ret 8) and +0x4cdd0e (ret 0Ch)
        ".globl _group_ret08\n_group_ret08:\n"
        "\tcall *_g_groupInc\n"
        "\tpopl %esi\n"
        "\tret $8\n"
        ".globl _group_ret0c\n_group_ret0c:\n"
        "\tcall *_g_groupInc\n"
        "\tpopl %esi\n"
        "\tret $0xc\n");

// For the test: the counter stubs by index.
__declspec(dllexport) void *CivProbe_BoundStub(int i)
{
    static void *const stubs[] = {(void *)cmd_ret0c, (void *)cmd_ret10, (void *)cmd_list,
                                  (void *)group_end, (void *)group_esi28, (void *)group_ret08, (void *)group_ret0c,
                                  (void *)cmd_ret08di};
    return i >= 0 && i < (int)(sizeof stubs / sizeof stubs[0]) ? stubs[i] : NULL;
}

// The counter sites, in the order of GameExe.bounds (DX11 addresses in the comments above).
typedef struct { BYTE len; BYTE bytes[35]; void *stub; } Site;
static const Site kBounds[8] = {
    {7, {0xff, 0x41, 0x0c, 0x5e, 0xc2, 0x0c, 0x00}, (void *)cmd_ret0c},
    {7, {0xff, 0x41, 0x0c, 0x5e, 0xc2, 0x10, 0x00}, (void *)cmd_ret10},
    {35, {0x8b, 0xb1, 0x7c, 0x20, 0x00, 0x00, 0x8b, 0xb9, 0x78, 0x20, 0x00, 0x00, 0xba, 0x01, 0x00, 0x00, 0x00,
          0x01, 0x51, 0x0c, 0x89, 0x04, 0xb7, 0x01, 0x91, 0x7c, 0x20, 0x00, 0x00, 0x5f, 0x5e, 0x5b, 0xc2, 0x08, 0x00},
     (void *)cmd_list},
    {15, {0x8b, 0x41, 0x04, 0x8d, 0x50, 0x01, 0xc6, 0x41, 0x18, 0x00, 0x89, 0x51, 0x04, 0x5e, 0xc3}, (void *)group_end},
    {13, {0x8b, 0x46, 0x04, 0x8d, 0x48, 0x01, 0x89, 0x4e, 0x04, 0x5e, 0xc2, 0x28, 0x00}, (void *)group_esi28},
    {13, {0x8b, 0x41, 0x04, 0x8d, 0x50, 0x01, 0x89, 0x51, 0x04, 0x5e, 0xc2, 0x08, 0x00}, (void *)group_ret08},
    {13, {0x8b, 0x41, 0x04, 0x8d, 0x50, 0x01, 0x89, 0x51, 0x04, 0x5e, 0xc2, 0x0c, 0x00}, (void *)group_ret0c},
    {13, {0x8b, 0x41, 0x04, 0x8d, 0x50, 0x01, 0x89, 0x51, 0x04, 0x5e, 0xc2, 0x08, 0x00}, (void *)group_ret08},
};
// One set of arrays (DX9): the third command appender ends without the list.
static const Site kBound1List = {8, {0xff, 0x41, 0x0c, 0x5f, 0x5e, 0xc2, 0x08, 0x00}, (void *)cmd_ret08di};

static const Site *bound_site(const GameExe *g, int i) { return i == 2 && g->streamHalves == 1 ? &kBound1List : &kBounds[i]; }

static int code_at(BYTE *exe, DWORD rva, const BYTE *want, DWORD len)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    return rva && rva + len <= nt->OptionalHeader.SizeOfImage && memcmp(exe + rva, want, len) == 0;
}

// Writes `jmp|call stub` + NOPs over len bytes at rva.
static int write_branch(DWORD rva, DWORD len, BYTE op, void *stub)
{
    BYTE *f = g_exe + rva, code[40];
    memset(code, 0x90, sizeof code);
    code[0] = op;
    LONG rel = (LONG)((BYTE *)stub - (f + 5));
    memcpy(code + 1, &rel, 4);
    DWORD old;
    if (!VirtualProtect(f, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(f, code, len);
    VirtualProtect(f, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, len);
    return 1;
}
// ---- Frame flip stats -------------------------------------------------------------------------

void *g_flipOrig;
static LONG g_seenCmdDrop, g_seenGroupDrop, g_seenListDrop, g_seenDataDrop;

// The counter limit, as the stubs compute it.
static DWORD half_limit(BYTE *a, BYTE *b, int shift, DWORD gameCap)
{
    DWORD d = (DWORD)(a > b ? a - b : b - a) >> shift;
    return d > gameCap ? d - 1 : d;
}

// Called for each stream at the frame flip, before its halves swap: the counters hold the
// frame just built.
void __cdecl flip_note(BYTE *s)
{
    Stream *t = stream_find(s);
    if (!t && g_halves == 1) {
        stream_register1(s);
        t = stream_find(s);
    }
    if (!t) return;
    DWORD c = *(DWORD *)(s + 0xc), g = *(DWORD *)(s + 4), d = *(DWORD *)(s + 0x40);
    DWORD l = g_halves == 2 ? *(DWORD *)(s + 0x207c) : 0;
    t->frames++;
    if (c > t->maxCmds) t->maxCmds = c;
    if (g > t->maxGroups) t->maxGroups = g;
    if (d > t->maxData) t->maxData = d;
    if (l > t->maxList) t->maxList = l;
    int over = (t->cmds && c > t->cmds) || (t->groups && g > t->groups) || (t->data && d > t->data);
    DWORD cmdCap, groupCap;
    if (g_halves == 2) {
        cmdCap = half_limit(*(BYTE **)(s + 0x14), *(BYTE **)(s + 0x2070), 5, *(DWORD *)(s + 8));
        groupCap = half_limit(*(BYTE **)s, *(BYTE **)(s + 0x206c), 7, *(DWORD *)(s + 0x10));
    } else {
        cmdCap = cmd_limit1(s), groupCap = group_limit1(s);
        if (cmdCap == 0xFFFFFFFF) cmdCap = 0;
        if (groupCap == 0xFFFFFFFF) groupCap = 0;
    }
    int full = (cmdCap && c >= cmdCap) || (groupCap && g >= groupCap) || l >= 0x3ff;
    if (over) {
        t->overFrames++;
        if (!t->warnedOver) {
            t->warnedOver = 1;
            log_event("cmdstream: stream from exe+0x%lx went past the game's capacity: commands %lu/%lu, groups %lu/%lu, data %lu/%lu KB (spare %lu, %lu, %lu KB)",
                      (unsigned long)t->creator, (unsigned long)c, (unsigned long)t->cmds, (unsigned long)g, (unsigned long)t->groups,
                      (unsigned long)(d >> 10), (unsigned long)(t->data >> 10), (unsigned long)t->cmdSpare,
                      (unsigned long)t->groupSpare, (unsigned long)(t->dataSpare >> 10));
        }
    }
    if (full) {
        t->fullFrames++;
        // Streams of a few entries are full every frame by design; the drop line below covers them.
        if (!t->warnedFull && (cmdCap >= 16 || groupCap >= 16 || l >= 0x3ff)) {
            t->warnedFull = 1;
            log_event("cmdstream: stream from exe+0x%lx filled a half (commands %lu/%lu, groups %lu/%lu, list %lu/1023); further entries share one slot",
                      (unsigned long)t->creator, (unsigned long)c, (unsigned long)cmdCap, (unsigned long)g,
                      (unsigned long)groupCap, (unsigned long)l);
        }
    }
    LONG cd = g_nCmdDrop, gd = g_nGroupDrop, ld = g_nListDrop, dd = g_nDropped;
    if (cd != g_seenCmdDrop || gd != g_seenGroupDrop || ld != g_seenListDrop || dd != g_seenDataDrop) {
        if (!g_seenCmdDrop && !g_seenGroupDrop && !g_seenListDrop && !g_seenDataDrop)
            log_event("cmdstream: first entries past a full half (commands %ld, groups %ld, list %ld, records %ld)", cd, gd, ld, dd);
        g_seenCmdDrop = cd; g_seenGroupDrop = gd; g_seenListDrop = ld; g_seenDataDrop = dd;
    }
}

// The flip call site calls this with ecx = stream; it notes the frame and goes on to the flip.
void flip_stub(void);
__asm__(".text\n.globl _flip_stub\n_flip_stub:\n"
        "\tpushl %ecx\n"
        "\tpushl %ecx\n"
        "\tcall _flip_note\n"
        "\taddl $4, %esp\n"
        "\tpopl %ecx\n"
        "\tjmp *_g_flipOrig\n");

__declspec(dllexport) void CivProbe_TestFlip(BYTE *s) { flip_note(s); }  // for the test

// One set of arrays (DX9): the reset at the start of each frame (exe+0x4eca40, thiscall, ecx =
// stream; `xor eax,eax` and zeroes +0xc, +4, +0x40, +0x44, +0x48, +0x30, +0x54) jumps here: note
// the frame just built, then the same reset. Keeps edx, as the original does.
static const BYTE kReset1[24] = {0x33, 0xc0, 0x89, 0x41, 0x0c, 0x89, 0x41, 0x04, 0x89, 0x41, 0x40, 0x89,
                                 0x41, 0x44, 0x89, 0x41, 0x48, 0x89, 0x41, 0x30, 0x89, 0x41, 0x54, 0xc3};
void reset_stub(void);
__asm__(".text\n.globl _reset_stub\n_reset_stub:\n"
        "\tpushl %edx\n"
        "\tpushl %ecx\n"
        "\tpushl %ecx\n"
        "\tcall _flip_note\n"
        "\taddl $4, %esp\n"
        "\tpopl %ecx\n"
        "\tpopl %edx\n"
        "\txorl %eax, %eax\n"
        "\tmovl %eax, 0xc(%ecx)\n"
        "\tmovl %eax, 4(%ecx)\n"
        "\tmovl %eax, 0x40(%ecx)\n"
        "\tmovl %eax, 0x44(%ecx)\n"
        "\tmovl %eax, 0x48(%ecx)\n"
        "\tmovl %eax, 0x30(%ecx)\n"
        "\tmovl %eax, 0x54(%ecx)\n"
        "\tret\n");

// ---- Record allocator -----------------------------------------------------------------------

#define MAX_POOLS 64
#define POOL_BYTES (4u << 20)
typedef struct { BYTE *s, *mem; DWORD used, lastUse; } Pool;
#define POOL_IDLE_MS 10000  // a pool unused this long may serve another stream (its own may be gone)
static Pool g_pools[MAX_POOLS];
static volatile LONG g_poolLock;

static void pool_lock(void) { while (InterlockedExchange(&g_poolLock, 1)) YieldProcessor(); }
static void pool_unlock(void) { InterlockedExchange(&g_poolLock, 0); }

static Pool *pool_of(BYTE *s, int create)
{
    for (int i = 0; i < MAX_POOLS; i++)
        if (g_pools[i].s == s) return &g_pools[i];
    if (!create) return NULL;
    // An idle pool first: streams that are made anew (at new addresses) don't each keep 4 MB.
    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_POOLS; i++)
        if (g_pools[i].s && now - g_pools[i].lastUse > POOL_IDLE_MS) {
            g_pools[i].s = s;
            g_pools[i].used = 0;
            return &g_pools[i];
        }
    for (int i = 0; i < MAX_POOLS; i++)
        if (!g_pools[i].s) {
            BYTE *m = VirtualAlloc(NULL, POOL_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!m) return NULL;
            g_pools[i].s = s;
            g_pools[i].mem = m;
            g_pools[i].used = 0;
            return &g_pools[i];
        }
    return NULL;
}

// A new frame for this stream: its pool (if it has one) starts over.
static void pool_reset(BYTE *s)
{
    pool_lock();
    Pool *pl = pool_of(s, 0);
    if (pl) pl->used = 0;
    pool_unlock();
}

static BYTE *pool_take(BYTE *s, DWORD sz)
{
    BYTE *p = NULL;
    pool_lock();
    Pool *pl = pool_of(s, 1);
    if (pl && pl->used + sz <= POOL_BYTES) {
        p = pl->mem + pl->used;
        pl->used += (sz + 15) & ~15u;
        pl->lastUse = GetTickCount();
    }
    pool_unlock();
    return p;
}

// Same contract as exe+0x4cfdc0: stdcall (stream, size), returns the record pointer and
// leaves it in [s+0x30], the size in [s+0x4C] (the tablet's rounds the size up to 256 bytes
// first). The limit is the distance between the two half bases (half + spare; the whole half is
// usable, as in the game), or with one set of arrays the size plus the spare the constructor's
// allocation got (looked up only once the size is reached). Records past it get pool
// memory of their own (records must not share memory) and the offset does not move; only if
// the pool is exhausted (or can't be allocated) do they share one static 64 KB slot, which fits
// any record (sizes are 16-bit). Also exported for the test.
static __attribute__((aligned(16))) BYTE g_scratch[0x10000];

__declspec(dllexport) BYTE *__stdcall CivProbe_StreamAlloc(BYTE *s, DWORD size)
{
    DWORD off = *(DWORD *)(s + 0x40), cap = *(DWORD *)(s + 0x34);
    BYTE *base = *(BYTE **)(s + 0x24);
    WORD sz = g_round ? (WORD)((size + 0xff) & 0xff00) : (WORD)size;
    BYTE *p;
    if (off == 0) pool_reset(s);
    DWORD limit = cap;
    if (g_halves == 2) {
        BYTE *other = *(BYTE **)(s + 0x2068);
        limit = (DWORD)(other > base ? other - base : base - other);
        if (limit < cap) limit = cap;
    } else if (base && off + sz > cap) {
        limit = cap + note_spare(base, NULL);
    }
    if (limit && off + sz > limit) {
        p = pool_take(s, sz);
        if (!p) { p = g_scratch; InterlockedIncrement(&g_nScratch); }  // last resort
        if (InterlockedIncrement(&g_nDropped) == 1)
            log_event("cmdstream: a command stream half (%lu KB + %lu KB spare) filled up; extra records are left out of the frame",
                      (unsigned long)(cap >> 10), (unsigned long)((limit - cap) >> 10));
    } else {
        p = base + off;
        *(DWORD *)(s + 0x40) = off + sz;
    }
    *(BYTE **)(s + 0x30) = p;
    *(WORD *)(s + 0x4C) = sz;
    return p;
}

// ---- Allocation wrappers ----------------------------------------------------------------------

// Which constructor allocation this is (0 none, 1 commands, 2 groups, 3 data) and the
// constructor's own return address. The caller's frames start at the wrapper's first argument
// (see watch.c on why not &n). From the slot holding the allocation's return address: 6
// arguments (12 for the groups: the commands' are still pushed), ebx/esi/edi, then the
// constructor's return address.
static int ctor_alloc(const DWORD *args, ULONG_PTR *ctorRet)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    const DWORD *top = (const DWORD *)tib->StackBase;
    for (int i = 0; i < 48 && args + i < top; i++)
        for (int k = 0; k < 3; k++)
            if (args[i] == g_rets[k]) {
                const DWORD *at = args + i + (k == 1 ? 16 : 10);
                *ctorRet = at < top ? *at : 0;
                return k + 1;
            }
    return 0;
}

// The room past the arrays: the slack, and with one set of arrays at least one entry's worth,
// because a full counter's writes go to the slot just past the game's capacity.
static size_t tail(void) { return g_halves == 1 && g_slack < 256 ? 256 : g_slack; }

// The spare (per half) and the note or grant for it.
static void *ctor_allocated(void *p, DWORD spare, ULONG_PTR ret)
{
    if (p && g_halves == 1) note_add(p, spare, ret);
    else if (p && spare) grant_add(p, spare);
    return p;
}

static void *__cdecl c_malloc(size_t n)
{
    ULONG_PTR ret;
    int kind = n >= 256 ? ctor_alloc((const DWORD *)__builtin_frame_address(0) + 2, &ret) : 0;
    if (!kind) return o_malloc(n);
    InterlockedIncrement(&g_nPadded);
    DWORD spare = 0;  // per half; n = g_halves x the half
    if (g_splitOn && spared(ret)) spare = (DWORD)(n / g_halves) * (kind == 1 ? g_cmdSpare : kind == 2 ? g_groupSpare : 0);
    return ctor_allocated(o_malloc(n + g_halves * (size_t)spare + tail()), spare, ret);
}

static void *__cdecl c_amalloc(size_t n, size_t align)
{
    ULONG_PTR ret;
    int kind = n >= 256 ? ctor_alloc((const DWORD *)__builtin_frame_address(0) + 2, &ret) : 0;
    if (!kind) return o_amalloc(n, align);
    InterlockedIncrement(&g_nPadded);
    DWORD spare = g_splitOn && kind == 3 && spared(ret) ? spare_for((DWORD)(n / g_halves)) : 0;
    return ctor_allocated(o_amalloc(n + g_halves * (size_t)spare + tail(), align), spare, ret);
}

// Wraps the exe's malloc and _aligned_malloc imports by name, chaining to whatever the
// slots hold now (msvcr90, or mialloc's replacements).
static int patch_exe(BYTE *exe)
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
            if (!strcmp(name, "malloc") && !o_malloc) { o_malloc = (malloc_t)(ULONG_PTR)slots->u1.Function; to = (void *)c_malloc; }
            else if (!strcmp(name, "_aligned_malloc") && !o_amalloc) { o_amalloc = (amalloc_t)(ULONG_PTR)slots->u1.Function; to = (void *)c_amalloc; }
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
// ---- Patching -----------------------------------------------------------------------------------

// The checks, each on its own so a part that doesn't match leaves the others on.
static int ctor_ok(BYTE *exe, const GameExe *g)
{
    if (!code_at(exe, g->streamCtor, kCtorStart, sizeof kCtorStart)) return 0;
    // the group entry size: mov edx,80h (two halves, exe+0x4ce1a4) / mov edx,90h (one set, DX9 exe+0x4ed4e0)
    static const BYTE kGroup2[] = {0xBA, 0x80, 0x00, 0x00, 0x00, 0xF7, 0xE2}, kGroup1[] = {0xBA, 0x90, 0x00, 0x00, 0x00, 0xF7, 0xE2};
    if (g->streamHalves == 2 ? !code_at(exe, g->streamCtor + 0x54, kGroup2, sizeof kGroup2)
                             : !code_at(exe, g->streamCtor + 0x50, kGroup1, sizeof kGroup1))
        return 0;
    for (int i = 0; i < 3; i++)  // each a return address inside the constructor, after a call
        if (g->ctorRets[i] <= g->streamCtor || g->ctorRets[i] - g->streamCtor > 0x100 || exe[g->ctorRets[i] - 5] != 0xE8)
            return 0;
    return 1;
}

static int split_ok(BYTE *exe, const GameExe *g)
{
    return g->streamHalves == 2 && code_at(exe, g->streamSplit, kSplit, sizeof kSplit) &&
           code_at(exe, g->streamHalvesSite, kHalves, sizeof kHalves);
}

// The spare callers: return addresses of calls to the stream factory, which allocates the stream
// and jumps to the constructor (`mov ecx,eax; jmp ctor` at +0x21), so the constructor returns there.
static int callers_ok(BYTE *exe, const GameExe *g)
{
    for (int i = 0; i < 3; i++) {
        const BYTE *f = g->spareCallers[i] ? exe_branch_target(exe, exe + g->spareCallers[i] - 5) : NULL;
        if (!f || exe[g->spareCallers[i] - 5] != 0xE8 || f[0x21] != 0x8B || f[0x22] != 0xC8 || f[0x23] != 0xE9 ||
            exe_branch_target(exe, f + 0x23) != exe + g->streamCtor)
            return 0;
    }
    return 1;
}

static int bump_ok(BYTE *exe, const GameExe *g)
{
    return g->recordRound ? code_at(exe, g->streamBump, kBumpRound, sizeof kBumpRound) : code_at(exe, g->streamBump, kBump, sizeof kBump);
}

static int bounds_ok(BYTE *exe, const GameExe *g, int *bad)
{
    for (int i = 0; i < 8; i++) {
        const Site *t = bound_site(g, i);
        if (!code_at(exe, g->bounds[i], t->bytes, t->len)) {
            *bad = i;
            return 0;
        }
    }
    return 1;
}

static int stats_ok(BYTE *exe, const GameExe *g)
{
    if (g->streamHalves == 1) return code_at(exe, g->streamReset, kReset1, sizeof kReset1);
    return g->flipCall && exe_branch_target(exe, exe + g->flipCall) == exe + g->flip && exe[g->flipCall] == 0xE8;
}

const char *cmdstream_check(BYTE *exe, const GameExe *g)
{
    static char why[96];
    int bad = 0;
    if (!ctor_ok(exe, g)) return "constructor";
    if (g->streamHalves == 2 && !split_ok(exe, g)) return "half layout";
    if (!callers_ok(exe, g)) return "spare callers";
    if (!bump_ok(exe, g)) return "record allocator";
    if (!bounds_ok(exe, g, &bad)) {
        wsprintfA(why, "counter site %d (exe+0x%lx)", bad, (unsigned long)g->bounds[bad]);
        return why;
    }
    if (!stats_ok(exe, g)) return g->streamHalves == 1 ? "reset" : "frame flip call";
    return NULL;
}

static void split_streams(void)
{
    if (g_halves == 2) {
        if (!split_ok(g_exe, g_g)) {
            log_event("cmdstream: half layout code not found at exe+0x%lx / +0x%lx; layout unchanged",
                      (unsigned long)g_g->streamSplit, (unsigned long)g_g->streamHalvesSite);
            return;
        }
        if (!write_branch(g_g->streamHalvesSite, sizeof kHalves, 0xE8, (void *)halves_stub)) return;
        if (!write_branch(g_g->streamSplit, sizeof kSplit, 0xE8, (void *)split_stub)) return;
    }
    g_splitOn = 1;  // one set of arrays: the wrappers alone make the room
    char list[96] = "";
    for (int i = 0, o = 0; i < g_nSpareCallers && o < 80; i++)
        o += wsprintfA(list + o, i ? ", exe+0x%lx" : "exe+0x%lx", (unsigned long)(g_spareCallers[i] - (ULONG_PTR)g_exe));
    log_event("cmdstream: streams created from %s get spare %s (commands +%lux, groups +%lux, data +min(%s, 1 MB))",
              list[0] ? list : "(none)", g_halves == 2 ? "halves" : "room", (unsigned long)g_cmdSpare,
              (unsigned long)g_groupSpare, g_halves == 2 ? "half" : "size");
}

static void cap_streams(void)
{
    int bad = 0;
    if (!bump_ok(g_exe, g_g)) {
        log_event("cmdstream: record allocator not found at exe+0x%lx; not capped", (unsigned long)g_g->streamBump);
        return;
    }
    if (!write_branch(g_g->streamBump, 5, 0xE9, (void *)CivProbe_StreamAlloc)) return;
    g_capOn = 1;
    if (!bounds_ok(g_exe, g_g, &bad)) {
        log_event("cmdstream: counter code not found at exe+0x%lx; counters not bounded", (unsigned long)g_g->bounds[bad]);
    } else {
        if (g_halves == 1) {
            g_cmdInc = (void *)cmd_inc1;
            g_groupInc = (void *)group_inc1;
        }
        for (int i = 0; i < 8; i++) write_branch(g_g->bounds[i], bound_site(g_g, i)->len, 0xE9, bound_site(g_g, i)->stub);
        g_boundOn = 1;
    }
    log_event("cmdstream: record allocator exe+0x%lx stops at each %s capacity%s", (unsigned long)g_g->streamBump,
              g_halves == 2 ? "half's" : "stream's", g_boundOn ? (g_halves == 2 ? "; command, group and list counters too" : "; command and group counters too") : "");
}

static void stats_streams(void)
{
    if (!stats_ok(g_exe, g_g)) {
        log_event("cmdstream: frame %s not found; no stream stats", g_halves == 2 ? "flip call" : "reset");
        return;
    }
    if (g_halves == 1) {
        if (!write_branch(g_g->streamReset, 5, 0xE9, (void *)reset_stub)) return;
    } else {
        g_flipOrig = g_exe + g_g->flip;
        if (!write_branch(g_g->flipCall, 5, 0xE8, (void *)flip_stub)) return;
    }
    g_statsOn = 1;
}

static void enlarge_pair(int mb)
{
    static const BYTE want[5] = {0x68, 0x00, 0x00, 0x10, 0x00};
    for (int i = 0; i < 2; i++)
        if (!code_at(g_exe, g_g->pairPush[i], want, 5)) {
            log_event("cmdstream: unexpected bytes at exe+0x%lx; size not changed", (unsigned long)g_g->pairPush[i]);
            return;
        }
    DWORD size = (DWORD)mb << 20;
    for (int i = 0; i < 2; i++) {
        BYTE *imm = g_exe + g_g->pairPush[i] + 1;
        DWORD old;
        if (!VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old)) return;
        memcpy(imm, &size, 4);
        VirtualProtect(imm, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), imm, 4);
    }
    log_event("cmdstream: command stream pair (exe+0x%lx) enlarged to %d MB per half", (unsigned long)g_g->spareCallers[0] - 5, mb);
}

// "rva,rva,..." -> absolute addresses in the exe.
static int parse_rvas(const WCHAR *s, ULONG_PTR *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        WCHAR *e;
        unsigned long v = wcstoul(s, &e, 0);
        if (e == s) break;
        out[n++] = (ULONG_PTR)g_exe + v;
        s = e;
        while (*s == L',' || *s == L' ') s++;
    }
    return n;
}

void cmdstream_init(const WCHAR *ini)
{
    g_exe = (BYTE *)GetModuleHandleW(NULL);
    g_g = game_exe(g_exe);
    WCHAR test[64];  // the test exe names its own return addresses: "rva,rva,rva"
    GetPrivateProfileStringW(L"probe", L"CommandStreamTestRets", L"", test, 64, ini);
    if (!test[0]) {
        if (!g_g) {
            log_event("cmdstream: not a known build of the game exe; off");
            return;
        }
        if (!ctor_ok(g_exe, g_g)) {
            log_event("cmdstream: command stream constructor not found at exe+0x%lx; off", (unsigned long)g_g->streamCtor);
            return;
        }
        g_halves = g_g->streamHalves;
        g_round = g_g->recordRound;
    }
    int mb = GetPrivateProfileIntW(L"probe", L"CommandStreamMB", 0, ini);
    if (mb > 1 && g_g && g_halves == 2) enlarge_pair(mb > 16 ? 16 : mb);

    WCHAR buf[128], def[64] = L"";
    if (g_g && callers_ok(g_exe, g_g))
        wsprintfW(def, L"0x%lx,0x%lx,0x%lx", (unsigned long)g_g->spareCallers[0], (unsigned long)g_g->spareCallers[1],
                  (unsigned long)g_g->spareCallers[2]);
    GetPrivateProfileStringW(L"probe", L"CommandStreamSpareCallers", def, buf, 128, ini);
    g_nSpareCallers = parse_rvas(buf, g_spareCallers, MAX_SPARE_CALLERS);
    g_cmdSpare = GetPrivateProfileIntW(L"probe", L"CommandStreamCmdSpare", 3, ini);
    g_groupSpare = GetPrivateProfileIntW(L"probe", L"CommandStreamGroupSpare", 1, ini);
    if (g_cmdSpare > 15) g_cmdSpare = 15;
    if (g_groupSpare > 15) g_groupSpare = 15;

    g_slack = (size_t)GetPrivateProfileIntW(L"probe", L"CommandStreamSlackKB", 4, ini) << 10;
    if (test[0]) parse_rvas(test, g_rets, 3);
    else
        for (int i = 0; i < 3; i++) g_rets[i] = (ULONG_PTR)g_exe + g_g->ctorRets[i];
    int n = patch_exe(g_exe);
    if (!o_malloc || !o_amalloc) { log_event("cmdstream: exe malloc/_aligned_malloc imports not found (%d); off", n); return; }
    if (g_slack) log_event("cmdstream: command stream buffers get %lu KB of slack past their end", (unsigned long)(g_slack >> 10));
    if (test[0]) {  // the test drives the stubs itself
        g_splitOn = GetPrivateProfileIntW(L"probe", L"CommandStreamSplit", 1, ini);
        return;
    }
    if (GetPrivateProfileIntW(L"probe", L"CommandStreamSplit", 1, ini)) split_streams();
    if (GetPrivateProfileIntW(L"probe", L"CommandStreamCap", 1, ini)) cap_streams();
    if (GetPrivateProfileIntW(L"probe", L"CommandStreamStats", 1, ini)) stats_streams();
}

// For the test: the one-set layout (DX9) for the allocator and counter stubs.
__declspec(dllexport) void CivProbe_TestLayout(int halves, int round)
{
    g_halves = halves;
    g_round = round;
    g_cmdInc = halves == 1 ? (void *)cmd_inc1 : (void *)cmd_inc;
    g_groupInc = halves == 1 ? (void *)group_inc1 : (void *)group_inc;
}

void cmdstream_report(void)
{
    if (o_malloc) log_event("cmdstream: %ld command stream buffers padded", g_nPadded);
    if (g_capOn)
        log_event("cmdstream: past a full half: %ld records (%ld in the shared scratch slot), %ld commands, %ld groups, %ld list entries",
                  g_nDropped, g_nScratch, g_nCmdDrop, g_nGroupDrop, g_nListDrop);
    if (!g_statsOn) return;
    LONG n = g_nStreams;
    for (LONG i = 0; i < n; i++) {
        Stream *t = &g_streams[i];
        if (!t->frames || (!t->maxCmds && !t->maxGroups && !t->maxData)) continue;
        log_event("cmdstream: stream exe+0x%lx: peak commands %lu/%lu (+%lu), groups %lu/%lu (+%lu), data %lu/%lu KB (+%lu), list %lu; %lu frames, %lu over the game's capacity, %lu full",
                  (unsigned long)t->creator, (unsigned long)t->maxCmds, (unsigned long)t->cmds, (unsigned long)t->cmdSpare,
                  (unsigned long)t->maxGroups, (unsigned long)t->groups, (unsigned long)t->groupSpare,
                  (unsigned long)(t->maxData >> 10), (unsigned long)(t->data >> 10), (unsigned long)(t->dataSpare >> 10),
                  (unsigned long)t->maxList, (unsigned long)t->frames, (unsigned long)t->overFrames, (unsigned long)t->fullFrames);
    }
}