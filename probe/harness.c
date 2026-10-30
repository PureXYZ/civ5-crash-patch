// Test harness for civprobe. Built as CivilizationV_probetest.exe so the probe
// activates; imports version.dll statically, like the game does.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef int (*SelfTest_t)(const WCHAR *, char *, int);

static LONG CALLBACK resume_after_raise(EXCEPTION_POINTERS *ep)
{
    // Our test exception comes from RaiseException, so execution can simply continue.
    return ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_CONTINUE_EXECUTION
                                                                             : EXCEPTION_CONTINUE_SEARCH;
}

int wmain(int argc, WCHAR **argv)
{
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);

    // 1. Forwarding works: these go through the proxy's jmp stubs.
    DWORD h, size = GetFileVersionInfoSizeW(L"C:\\Windows\\SysWOW64\\kernel32.dll", &h);
    BYTE *buf = (BYTE *)malloc(size);
    VS_FIXEDFILEINFO *ffi = NULL;
    UINT len = 0;
    BOOL ok = size && GetFileVersionInfoW(L"C:\\Windows\\SysWOW64\\kernel32.dll", 0, size, buf) &&
              VerQueryValueW(buf, L"\\", (void **)&ffi, &len);
    printf("forwarding: size=%lu ok=%d version=%u.%u.%u\n", size, ok,
           ffi ? HIWORD(ffi->dwFileVersionMS) : 0, ffi ? LOWORD(ffi->dwFileVersionMS) : 0,
           ffi ? HIWORD(ffi->dwFileVersionLS) : 0);

    // msimg32 forwarding: fill a small memory bitmap through GradientFill.
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateCompatibleBitmap(dc, 8, 8);
    SelectObject(dc, bmp);
    TRIVERTEX v[2] = {{0, 0, 0xff00, 0, 0, 0}, {8, 8, 0, 0, 0xff00, 0}};
    GRADIENT_RECT gr = {0, 1};
    printf("forwarding: GradientFill=%d\n", GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H));

    // 2. Lua allocator scan against real game executables passed on the command line.
    HMODULE probe = GetModuleHandleW(L"msimg32.dll");
    if (!GetProcAddress(probe, "CivProbe_SelfTest")) probe = GetModuleHandleW(L"version.dll");
    SelfTest_t selfTest = (SelfTest_t)GetProcAddress(probe, "CivProbe_SelfTest");
    printf("probe module=%p selftest=%p\n", (void *)probe, (void *)selfTest);
    SelfTest_t checkExe = (SelfTest_t)GetProcAddress(probe, "CivProbe_CheckExe");
    for (int i = 1; i < argc && selfTest; i++) {
        char report[512];
        int found = selfTest(argv[i], report, sizeof report);
        printf("scan %ls: found=%d %s\n", argv[i], found, report);
        if (!checkExe) continue;
        found = checkExe(argv[i], report, sizeof report);
        printf("check %ls: ok=%d %s\n", argv[i], found, report);
    }

    // 3. Give the sampler something to see: reserve and commit some memory.
    void *r = VirtualAlloc(NULL, 256 << 20, MEM_RESERVE, PAGE_READWRITE);
    void *c = VirtualAlloc(NULL, 64 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE heap = HeapCreate(0, 1 << 20, 0);
    for (int i = 0; i < 1000; i++) HeapAlloc(heap, 0, 16 * 1024);
    printf("reserved=%p committed=%p heap=%p\n", r, c, heap);
    Sleep(5000);

    // 4. Exception path: the probe logs and snapshots, then we resume.
    AddVectoredExceptionHandler(0, resume_after_raise);
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    printf("survived test exception\n");
    Sleep(2500);
    return 0;
}
