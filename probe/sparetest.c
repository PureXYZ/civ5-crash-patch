// Spare-halves allocation test. Fake constructors reproduce the stack shape of the game's
// command stream constructor (exe+0x4ce150) at its three allocations (commands, groups, data),
// reached from "factory callers" (call_*). The parent run writes civprobe.ini naming this
// exe's own sites (CommandStreamTestRets = the allocations' return addresses,
// CommandStreamSpareCallers = three of the callers), runs itself as the child, and restores
// the ini. The child checks that allocations from spared callers are enlarged by the spare
// areas (+ slack), others only by the slack, and that the constructor stubs lay out the second
// halves from those grants.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

// The child also runs the FAudioGame watch (Watch=1), which wraps malloc too and has a big
// stack frame: the stream wrapper must still find the constructor (session 20261029_003228).
__declspec(dllexport) void *volatile g_sparewatched;

__attribute__((noinline)) void *wrap_malloc(size_t n) { void *volatile p = malloc(n); return p; }
__attribute__((noinline)) void *wrap_amalloc(size_t n, size_t align) { void *volatile p = _aligned_malloc(n, 16); return p; }

void *call_a1(size_t), *call_a1b(size_t), *call_a2(size_t), *call_data(size_t);
extern char mark_a1[], mark_a2[], mark_data[], ret_a1[], ret_a2[], ret_data[];
__asm__(".text\n"
        // commands: ebx/esi/edi, then 6 arguments (size last), call
        "_fake_a1:\n\tpushl %ebx\n\tpushl %esi\n\tpushl %edi\n"
        "\tpushl $0xc\n\tpushl $9\n\tpushl $0x18\n\tpushl $0\n\tpushl $1\n\tpushl 0x24(%esp)\n"
        "\tcall _wrap_malloc\n"
        ".globl _mark_a1\n_mark_a1:\n\taddl $0x18, %esp\n\tpopl %edi\n\tpopl %esi\n\tpopl %ebx\n\tret\n"
        // groups: the commands' 6 arguments are still pushed
        "_fake_a2:\n\tpushl %ebx\n\tpushl %esi\n\tpushl %edi\n"
        "\tpushl $0xc\n\tpushl $9\n\tpushl $0x18\n\tpushl $0\n\tpushl $1\n\tpushl $0x77\n"
        "\tpushl $0xd\n\tpushl $9\n\tpushl $0x19\n\tpushl $0\n\tpushl $1\n\tpushl 0x3c(%esp)\n"
        "\tcall _wrap_malloc\n"
        ".globl _mark_a2\n_mark_a2:\n\taddl $0x30, %esp\n\tpopl %edi\n\tpopl %esi\n\tpopl %ebx\n\tret\n"
        // data: aligned allocation (size, 16, ...)
        "_fake_data:\n\tpushl %ebx\n\tpushl %esi\n\tpushl %edi\n"
        "\tpushl $0xe\n\tpushl $9\n\tpushl $0x1c\n\tpushl $0\n\tpushl $0x10\n\tpushl 0x24(%esp)\n"
        "\tcall _wrap_amalloc\n"
        ".globl _mark_data\n_mark_data:\n\taddl $0x18, %esp\n\tpopl %edi\n\tpopl %esi\n\tpopl %ebx\n\tret\n"
        // the "factory callers": the constructor's return address is the label after each call
        ".globl _call_a1\n_call_a1:\n\tpushl 4(%esp)\n\tcall _fake_a1\n.globl _ret_a1\n_ret_a1:\n\taddl $4, %esp\n\tret\n"
        ".globl _call_a1b\n_call_a1b:\n\tpushl 4(%esp)\n\tcall _fake_a1\n\taddl $4, %esp\n\tret\n"
        ".globl _call_a2\n_call_a2:\n\tpushl 4(%esp)\n\tcall _fake_a2\n.globl _ret_a2\n_ret_a2:\n\taddl $4, %esp\n\tret\n"
        ".globl _call_data\n_call_data:\n\tpushl 4(%esp)\n\tcall _fake_data\n.globl _ret_data\n_ret_data:\n\taddl $4, %esp\n\tret\n");

static int g_ok = 1;
static void check(int good, const char *what) { printf("%-58s %s\n", what, good ? "ok" : "WRONG"); if (!good) g_ok = 0; }

static int parent(void)
{
    char exe[MAX_PATH], ini[MAX_PATH], *slash;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(ini, exe);
    slash = strrchr(ini, '\\');
    strcpy(slash + 1, "civprobe\\civprobe.ini");
    static char saved[65536];
    FILE *f = fopen(ini, "rb");
    size_t n = f ? fread(saved, 1, sizeof saved, f) : 0;
    if (f) fclose(f);
    ULONG_PTR b = (ULONG_PTR)GetModuleHandleA(NULL);
    f = fopen(ini, "wb");
    if (!f) { printf("cannot write %s\n", ini); return 1; }
    fprintf(f, "[probe]\r\nAllocTracking=0\r\nIntervalMs=1000\r\nWatch=1\r\nWatchGlobalExport=g_sparewatched\r\nCommandStreamTestRets=0x%lx,0x%lx,0x%lx\r\nCommandStreamSpareCallers=0x%lx,0x%lx,0x%lx\r\n",
            (unsigned long)((ULONG_PTR)mark_a1 - b), (unsigned long)((ULONG_PTR)mark_a2 - b), (unsigned long)((ULONG_PTR)mark_data - b),
            (unsigned long)((ULONG_PTR)ret_a1 - b), (unsigned long)((ULONG_PTR)ret_a2 - b), (unsigned long)((ULONG_PTR)ret_data - b));
    fclose(f);
    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof cmd, "\"%s\" child", exe);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    f = fopen(ini, "wb");  // put the shared test ini back
    if (f) { fwrite(saved, 1, n, f); fclose(f); }
    return (int)code;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    if (argc < 2) return parent();
    HMODULE probe = GetModuleHandleA("msimg32.dll");
    void *split = ((void *(*)(void))GetProcAddress(probe, "CivProbe_SplitStub"))();
    void *halves = ((void *(*)(void))GetProcAddress(probe, "CivProbe_HalvesStub"))();
    const size_t slack = 4096;
    BYTE *c1 = call_a1(0x10000), *c2 = call_a1b(0x10000), *g1 = call_a2(0x8000), *d1 = call_data(0x200000);
    size_t sc1 = _msize(c1), sc2 = _msize(c2), sg1 = _msize(g1), sd1 = _aligned_msize(d1, 16, 0);
    printf("commands spared %zu, commands other caller %zu, groups %zu, data %zu\n", sc1, sc2, sg1, sd1);
    check(sc1 == 0x10000 + 2 * 3 * 0x8000 + slack, "commands from a spared caller: +3 halves per half + slack");
    check(sc2 == 0x10000 + slack, "commands from another caller: slack only");
    check(sg1 == 0x8000 + 2 * 0x4000 + slack, "groups from a spared caller: +1 half per half + slack");
    check(sd1 == 0x200000 + 2 * 0x100000 + slack, "data from a spared caller: +min(half, 1 MB) per half + slack");

    static BYTE s[0x2100];
    DWORD c = 0x8000 / 0x100, d = 0x10000 / 0x40;  // a2, a1
    *(BYTE **)(s + 0x14) = c1;
    *(BYTE **)s = g1;
    __asm__ volatile("pushl $0\n\tpushl $1\n\tpushl $2\n\tpushl $3\n\tpushl $4\n\tpushl $5\n\tpushl $6\n\tcall *%[f]\n\taddl $28, %%esp"
                     : "+c"(c), "+d"(d) : "S"(s), [f] "b"(halves) : "eax", "memory");
    check(d == (DWORD)(ULONG_PTR)(c1 + 0x8000 + 3 * 0x8000) && c == (DWORD)(ULONG_PTR)(g1 + 0x4000 + 0x4000),
          "constructor stub: command/group half 2 after the granted spares");
    c = 0x80; d = 0x400;
    *(BYTE **)(s + 0x14) = c2;
    __asm__ volatile("pushl $0\n\tpushl $1\n\tpushl $2\n\tpushl $3\n\tpushl $4\n\tpushl $5\n\tpushl $6\n\tcall *%[f]\n\taddl $28, %%esp"
                     : "+c"(c), "+d"(d) : "S"(s), [f] "b"(halves) : "eax", "memory");
    check(d == (DWORD)(ULONG_PTR)(c2 + 0x8000), "no grant (other caller): the game's command layout");
    BYTE *eaxOut = d1;
    DWORD half = 0x100000;
    __asm__ volatile("call *%[f]" : "+a"(eaxOut) : "S"(s), "D"(half), [f] "b"(split) : "ecx", "edx", "memory");
    check(*(BYTE **)(s + 0x2068) == d1 + 0x200000 && eaxOut == d1 + 0x100000, "constructor stub: data half 2 after the granted spare");
    printf("%s\n", g_ok ? "ALL PASSED" : "FAILURES");
    return g_ok ? 0 : 1;
}