// Command stream test: drives the probe's replacements for the game's command stream code on
// fake streams. Run as CivilizationV_streamtest.exe next to the probe (CommandStreamTestRets set,
// so the probe patches nothing and the test calls the stubs itself).
//  - CivProbe_StreamAlloc (what exe+0x4cfdc0 jumps to): records stay inside half + spare; past
//    it each record gets memory of its own and the offset stops.
//  - The constructor stubs (exe+0x4ce1e8 data, exe+0x4ce20a commands/groups): second halves go
//    after the first half's spare, taken from the allocation's grant (none: the game's layout).
//  - The counter stubs (exe+0x4ccdf9 etc.): counts stop at the last slot of the half, nothing
//    is written past it, and each returns with the stack and callee-saved registers intact.
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef BYTE *(__stdcall *alloc_t)(BYTE *, DWORD);
static HMODULE g_probe;
static int g_ok = 1;
static void *proc(const char *n) { void *p = (void *)GetProcAddress(g_probe, n); if (!p) { printf("export %s missing\n", n); g_ok = 0; } return p; }
static void check(int good, const char *what) { printf("%-60s %s\n", what, good ? "ok" : "WRONG"); if (!good) g_ok = 0; }

static BYTE g_s[0x2100];  // a fake stream object

// Enter a counter stub the way the game's function reaches it: `nargs` arguments and a return
// address on the stack, then the function's own saved registers (1: esi; 2: esi, edi; 3: ebx,
// esi, edi in the game's order), ecx = stream (esi = stream for group_esi28), eax = value. The parameters
// live in globals so the asm can address them while it moves esp.
static DWORD g_n, g_sv, g_es, g_espBefore, g_espAfter;
static void enter(void *stub, int nargs, int saves, int esiStream, DWORD value)
{
    DWORD a = value, c = (DWORD)(ULONG_PTR)g_s, d = (DWORD)(ULONG_PTR)stub;
    g_n = nargs; g_sv = saves; g_es = esiStream;
    __asm__ volatile(
        "movl %%esp, %[eb]\n\t"
        "pushl %%ebp\n\t"
        "movl %[n], %%ebp\n"
        "0:\ttestl %%ebp, %%ebp\n\t"
        "jz 1f\n\t"
        "pushl $0x5a5a5a5a\n\t"
        "decl %%ebp\n\t"
        "jmp 0b\n"
        "1:\tmovl %[sv], %%ebp\n\t"
        "call 2f\n\t"
        "jmp 5f\n"
        "2:\tcmpl $1, %%ebp\n\t"
        "jb 4f\n\t"
        "ja 3f\n\t"
        "pushl %%esi\n\t"
        "jmp 4f\n"
        "3:\tcmpl $2, %%ebp\n\t"
        "ja 7f\n\t"
        "pushl %%esi\n\t"
        "pushl %%edi\n\t"
        "jmp 4f\n"
        "7:\tpushl %%ebx\n\t"
        "pushl %%esi\n\t"
        "pushl %%edi\n"
        "4:\tcmpl $0, %[es]\n\t"
        "je 6f\n\t"
        "movl %%ecx, %%esi\n"
        "6:\tjmp *%%edx\n"
        "5:\tpopl %%ebp\n\t"
        "movl %%esp, %[ea]"
        : "+a"(a), "+c"(c), "+d"(d), [eb] "=m"(g_espBefore), [ea] "=m"(g_espAfter)
        : [n] "m"(g_n), [sv] "m"(g_sv), [es] "m"(g_es)
        : "memory", "cc");
    if (g_espBefore != g_espAfter) {
        printf("  stub %p: stack not balanced (esp %lx -> %lx)\n", stub, (unsigned long)g_espBefore, (unsigned long)g_espAfter);
        g_ok = 0;
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    g_probe = GetModuleHandleA("msimg32.dll");
    alloc_t f = (alloc_t)proc("CivProbe_StreamAlloc");
    void (*grant)(void *, DWORD) = (void (*)(void *, DWORD))proc("CivProbe_TestGrant");
    void *(*bound)(int) = (void *(*)(int))proc("CivProbe_BoundStub");
    if (!g_ok) return 1;
    BYTE *s = g_s;
    DWORD old;

    // 1. Records, the game's layout: half 2 right after half 1 (a NOACCESS page there).
    const DWORD cap = 1u << 20;
    BYTE *buf = VirtualAlloc(NULL, 2 * cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    VirtualProtect(buf + cap, 4096, PAGE_NOACCESS, &old);
    memset(s, 0, sizeof g_s);
    *(BYTE **)(s + 0x24) = buf;
    *(BYTE **)(s + 0x2068) = buf + cap;
    *(DWORD *)(s + 0x34) = cap;
    int fitted = 0, pooled = 0, bad = 0, aliased = 0;
    static BYTE *seen[4000];
    for (int frame = 0; frame < 2; frame++) {
        *(DWORD *)(s + 0x40) = 0;  // a new frame
        fitted = pooled = 0;
        for (int i = 0; i < 4000; i++) {
            BYTE *p = f(s, 0x160);
            memset(p, i, 0x160);  // write the whole record, like the game
            seen[i] = p;
            if (p >= buf && p < buf + cap) { fitted++; if (p + 0x160 > buf + cap) bad++; }
            else pooled++;
            if (*(BYTE **)(s + 0x30) != p || *(WORD *)(s + 0x4C) != 0x160) bad++;
        }
        for (int i = 0; i < 4000; i++)  // every record still holds its own data: no sharing
            if (seen[i][0] != (BYTE)i || seen[i][0x15F] != (BYTE)i) aliased++;
    }
    int expect = cap / 0x160;
    printf("records: fitted %d, own overflow memory %d, offset 0x%lx, bad %d, aliased %d\n", fitted, pooled,
           (unsigned long)*(DWORD *)(s + 0x40), bad, aliased);
    check(!bad && !aliased && fitted == expect && pooled == 4000 - expect && *(DWORD *)(s + 0x40) == (DWORD)expect * 0x160,
          "records stop at the half (game layout)");

    // 2. Records with a spare area: half 2 starts at half + spare.
    BYTE *buf2 = VirtualAlloc(NULL, 2 * cap + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    VirtualProtect(buf2 + 2 * cap, 4096, PAGE_NOACCESS, &old);
    memset(s, 0, sizeof g_s);
    *(BYTE **)(s + 0x24) = buf2;
    *(BYTE **)(s + 0x2068) = buf2 + 2 * cap;
    *(DWORD *)(s + 0x34) = cap;
    int inSpare = 0, beyond = 0, dropped = 0;
    for (int i = 0; i < 8000; i++) {
        BYTE *p = f(s, 0x160);
        memset(p, i, 0x160);
        if (p >= buf2 + cap && p < buf2 + 2 * cap) inSpare++;
        if (p >= buf2 && p + 0x160 > buf2 + 2 * cap) beyond++;
        if (p < buf2 || p >= buf2 + 2 * cap) dropped++;
    }
    int expect2 = 2 * cap / 0x160;
    printf("spare: %d records in the spare area, %d left out (expect %d), %d past it\n", inSpare, dropped, 8000 - expect2, beyond);
    check(!beyond && dropped == 8000 - expect2 && *(DWORD *)(s + 0x40) == (DWORD)expect2 * 0x160, "records use the spare area, then stop");

    // 3. Data split stub (esi = stream, eax = buffer, edi = half): with and without a grant.
    void *split = ((void *(*)(void))proc("CivProbe_SplitStub"))();
    for (int k = 0; k < 2; k++) {
        BYTE *bufp = (BYTE *)0x10000000 + k * 0x1000000, *eaxOut;
        DWORD half = 0x100000, spare = k ? 0x80000 : 0;
        if (spare) grant(bufp, spare);
        memset(s, 0, sizeof g_s);
        eaxOut = bufp;
        __asm__ volatile("call *%[f]" : "+a"(eaxOut) : "S"(s), "D"(half), [f] "b"(split) : "ecx", "edx", "memory");
        BYTE *h1 = *(BYTE **)(s + 0x24), *h2 = *(BYTE **)(s + 0x2068);
        printf("data split, spare 0x%lx: half 2 at +0x%lx\n", (unsigned long)spare, (unsigned long)(h2 - bufp));
        check(h1 == bufp && h2 == bufp + half + spare && eaxOut == bufp + half, k ? "data half 2 after the granted spare" : "no grant: the game's data layout");
    }

    // 4. Command/group halves stub (esi = stream, ecx = a2, edx = a1; ctor return at [esp+0x1c]).
    void *halves = ((void *(*)(void))proc("CivProbe_HalvesStub"))();
    for (int k = 0; k < 2; k++) {
        BYTE *cmds = (BYTE *)0x20000000 + k * 0x1000000, *groups = (BYTE *)0x30000000 + k * 0x1000000;
        DWORD a1 = 0x1000, a2 = 0x400, cs = k ? 3 * a1 * 0x20 : 0, gs = k ? a2 * 0x80 : 0;
        if (cs) grant(cmds, cs);
        if (gs) grant(groups, gs);
        memset(s, 0, sizeof g_s);
        *(BYTE **)(s + 0x14) = cmds;
        *(BYTE **)s = groups;
        DWORD c = a2, d = a1;
        __asm__ volatile("pushl $0x12345678\n\tpushl $1\n\tpushl $2\n\tpushl $3\n\tpushl $4\n\tpushl $5\n\tpushl $6\n\t"
                         "call *%[f]\n\taddl $28, %%esp"
                         : "+c"(c), "+d"(d) : "S"(s), [f] "b"(halves) : "eax", "memory");
        printf("halves, spare %lu/%lu: group half 2 +0x%lx, command half 2 +0x%lx\n", (unsigned long)gs, (unsigned long)cs,
               (unsigned long)(c - (DWORD)(ULONG_PTR)groups), (unsigned long)(d - (DWORD)(ULONG_PTR)cmds));
        check(c == (DWORD)(ULONG_PTR)(groups + a2 * 0x80 + gs) && d == (DWORD)(ULONG_PTR)(cmds + a1 * 0x20 + cs),
              k ? "command/group half 2 after the granted spares" : "no grant: the game's command/group layout");
    }

    // 5. Counter stubs: 64 command slots per half, 16 group slots, 1024 list slots + canary.
    static BYTE cmdArr[2 * 64 * 0x20 + 0x20], groupArr[2 * 16 * 0x80 + 0x80];
    static DWORD list[1025];
    struct { int idx, nargs, saves, esiStream, group; const char *name; } st[] = {
        {0, 3, 1, 0, 0, "commands (exe+0x4ccdf9, ret 0Ch)"},
        {1, 4, 1, 0, 0, "commands (exe+0x4cce7b, ret 10h)"},
        {2, 2, 3, 0, 0, "commands + list (exe+0x4ccefd, ret 8)"},
        {3, 0, 1, 0, 1, "groups (exe+0x4cdb30, ret)"},
        {4, 10, 1, 1, 1, "groups (exe+0x4cdc36, esi = stream, ret 28h)"},
        {5, 2, 1, 0, 1, "groups (exe+0x4cdc9e / +0x4cdd6e, ret 8)"},
        {6, 3, 1, 0, 1, "groups (exe+0x4cdd0e, ret 0Ch)"},
    };
    // With a spare area (game capacity 16 commands / 4 groups < the slots) counts stop at the
    // last slot; without (game capacity = the slots) they keep every slot, as in the game.
    for (int sp = 0; sp < 2; sp++)
    for (int k = 0; k < 7; k++) {
        memset(s, 0, sizeof g_s);
        *(DWORD *)(s + 8) = sp ? 16 : 64;
        *(DWORD *)(s + 0x10) = sp ? 4 : 16;
        memset(list, 0, sizeof list);
        list[1024] = 0xC0FFEE;
        *(BYTE **)(s + 0x14) = cmdArr; *(BYTE **)(s + 0x2070) = cmdArr + 64 * 0x20;
        *(BYTE **)s = groupArr; *(BYTE **)(s + 0x206c) = groupArr + 16 * 0x80;
        *(DWORD **)(s + 0x2078) = list;
        s[0x18] = 1;
        int calls = k == 2 ? 1100 : 100;
        for (int i = 0; i < calls; i++) enter(bound(st[k].idx), st[k].nargs, st[k].saves, st[k].esiStream, 1000 + i);
        DWORD cmdN = *(DWORD *)(s + 0xc), groupN = *(DWORD *)(s + 4), listN = *(DWORD *)(s + 0x207c);
        int good;
        DWORD cmdWant = sp ? 63 : 64, groupWant = sp ? 15 : 16;
        if (st[k].group) good = groupN == groupWant && cmdN == 0 && (k != 3 || s[0x18] == 0);
        else good = cmdN == cmdWant && groupN == 0;
        if (k == 2) good = good && listN == 1023 && list[0] == 1000 && list[1022] == 2022 && list[1023] == 1000 + 1099 && list[1024] == 0xC0FFEE;
        printf("%s%s: commands %lu, groups %lu, list %lu\n", sp ? "spare, " : "", st[k].name, (unsigned long)cmdN, (unsigned long)groupN, (unsigned long)listN);
        check(good, sp ? "  counts stop at the last spare slot; list at 1023" : "  counts keep the game's full capacity; list at 1023");
    }
    // A stream without an array (both half pointers equal) keeps the game's behaviour: no limit.
    memset(s, 0, sizeof g_s);
    for (int i = 0; i < 10; i++) enter(bound(0), 3, 1, 0, 0);
    check(*(DWORD *)(s + 0xc) == 10, "no command array: counter unbounded, as in the game");

    // 6. The tablet's record allocator: sizes rounded up to 256 bytes (16 bits, as the game's).
    void (*layout)(int, int) = (void (*)(int, int))proc("CivProbe_TestLayout");
    void (*note)(void *, DWORD) = (void (*)(void *, DWORD))proc("CivProbe_TestNote");
    if (!g_ok) return 1;
    layout(2, 1);
    memset(s, 0, sizeof g_s);
    *(BYTE **)(s + 0x24) = buf;
    *(BYTE **)(s + 0x2068) = buf + cap;
    *(DWORD *)(s + 0x34) = cap;
    BYTE *r1 = f(s, 0x160), *r2 = f(s, 0x100);
    WORD w2 = *(WORD *)(s + 0x4C);
    f(s, 0xff01);
    printf("tablet: records at +0x%lx, +0x%lx, size word 0x%x, offset 0x%lx\n", (unsigned long)(r1 - buf), (unsigned long)(r2 - buf),
           w2, (unsigned long)*(DWORD *)(s + 0x40));
    check(r1 == buf && r2 == buf + 0x200 && w2 == 0x100 && *(DWORD *)(s + 0x40) == 0x300 && *(WORD *)(s + 0x4C) == 0,
          "tablet: record sizes rounded up to 256 bytes");

    // 7. One set of arrays (DX9): records stop at the size, or at size + the noted spare.
    layout(1, 0);
    for (int k = 0; k < 2; k++) {
        BYTE *b = VirtualAlloc(NULL, 2 * cap + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD spare = k ? cap : 0;
        VirtualProtect(b + cap + spare, 4096, PAGE_NOACCESS, &old);
        note(b, spare);
        memset(s, 0, sizeof g_s);
        *(BYTE **)(s + 0x24) = b;
        *(DWORD *)(s + 0x34) = cap;
        *(BYTE **)(s + 0x2068) = (BYTE *)0x7ff00000;  // not a field in this layout: must not be used
        int in = 0, out = 0, past = 0;
        for (int i = 0; i < 8000; i++) {
            BYTE *p = f(s, 0x160);
            memset(p, i, 0x160);
            if (p >= b && p < b + cap + spare) { in++; if (p + 0x160 > b + cap + spare) past++; }
            else out++;
        }
        int want = (cap + spare) / 0x160;
        printf("one set, spare 0x%lx: %d records inside, %d own memory, %d past the end\n", (unsigned long)spare, in, out, past);
        check(in == want && out == 8000 - want && !past, k ? "one set: records use the noted spare, then stop" : "one set: records stop at the size");
    }

    // 8. One set of arrays: counters stop at the noted slots; no note: one past the game's capacity
    // (the slot in the slack), as with two halves and no spare. Groups are 0x90 bytes here.
    static BYTE cmd1[64 * 0x20], group1[16 * 0x90];
    struct { int idx, nargs, saves, esiStream, group; const char *name; } st1[] = {
        {0, 3, 1, 0, 0, "commands (DX9 exe+0x4ec289, ret 0Ch)"},
        {1, 4, 1, 0, 0, "commands (DX9 exe+0x4ec30b, ret 10h)"},
        {7, 2, 2, 0, 0, "commands (DX9 exe+0x4ec38c, pop edi/esi, ret 8)"},
        {3, 0, 1, 0, 1, "groups (DX9 exe+0x4ecee3, ret)"},
        {4, 10, 1, 1, 1, "groups (DX9 exe+0x4ed01a, esi = stream, ret 28h)"},
        {5, 2, 1, 0, 1, "groups (DX9 exe+0x4ed08d / +0x4ed17d, ret 8)"},
        {6, 3, 1, 0, 1, "groups (DX9 exe+0x4ed110, ret 0Ch)"},
    };
    for (int sp = 0; sp < 2; sp++)
    for (int k = 0; k < 7; k++) {
        memset(s, 0, sizeof g_s);
        *(DWORD *)(s + 8) = 16;
        *(DWORD *)(s + 0x10) = 4;
        *(BYTE **)(s + 0x14) = cmd1;
        *(BYTE **)s = group1;
        note(cmd1, sp ? 48 * 0x20 : 0);
        note(group1, sp ? 12 * 0x90 : 0);
        for (int i = 0; i < 100; i++) enter(bound(st1[k].idx), st1[k].nargs, st1[k].saves, st1[k].esiStream, 1000 + i);
        DWORD cmdN = *(DWORD *)(s + 0xc), groupN = *(DWORD *)(s + 4);
        int good = st1[k].group ? groupN == (sp ? 15u : 4u) && cmdN == 0 : cmdN == (sp ? 63u : 16u) && groupN == 0;
        printf("one set, %s%s: commands %lu, groups %lu\n", sp ? "spare, " : "", st1[k].name, (unsigned long)cmdN, (unsigned long)groupN);
        check(good, sp ? "  counts stop at the last noted slot" : "  counts stop one past the game's capacity");
    }
    memset(s, 0, sizeof g_s);
    *(DWORD *)(s + 8) = 16;
    for (int i = 0; i < 40; i++) enter(bound(0), 3, 1, 0, 0);
    check(*(DWORD *)(s + 0xc) == 40, "one set, no command array: counter unbounded, as in the game");
    layout(2, 0);

    printf("%s\n", g_ok ? "ALL PASSED" : "FAILURES");
    return g_ok ? 0 : 1;
}