// No intro video player ([probe] SkipIntroPlayer: 2 = never start it, the default; 1 = only when the
// game's own SkipIntroVideo is on; 0 = off).
//
// At startup the exe plays the opening movie through DirectShow (exe+0x1356d0 makes the player,
// exe+0x14dd70, copies the SkipIntroVideo setting (UserSettings.ini, exe+0x8cf0c5) into it at
// exe+0x135742, calls its Play, exe+0x21a340, and waits for its IsDone, exe+0x2b2b30, pumping
// messages). Play builds a filter graph (CoCreateInstance(CLSID_FilterGraph) at exe+0x21a3f4) with
// the VMR-7 renderer, which is DirectDraw 7. On Windows 10 and 11 DirectDraw runs on the GPU's
// Direct3D 9 driver, so DirectDraw, Direct3D 7 (D3DIM700) and the D3D9 driver (NVIDIA's
// nvd3dum.dll: a 65 MB image, at ~11 s in the loader's own load times) come in with it, plus
// DirectShow and the WMV decoders, and they stay loaded with a driver thread for the whole session
// after the player is released: ~75 MB of address space (2026-11-02, session 20261101_233101). With
// SkipIntroVideo on, the game still plays the movie's first 8.5 s (IsDone: position >= +0x38/+0x3c,
// 0x510ff40 x 100 ns) and then stops it.
//
// With SkipIntroPlayer=2, or 1 and the game's SkipIntroVideo on, the exe's CoCreateInstance import
// refuses that one call (CLSID_FilterGraph from the Play site) with REGDB_E_CLASSNOTREG, as on a PC
// without DirectShow. Play then returns at once with its finished flag (+0x2c) set, IsDone is true
// at the first check, and the owner calls the player's Stop as usual (it releases only what
// exists). The setting only matters inside IsDone, so the path is the same either way; with it
// off the movie would otherwise play to its end or a key press. The one thing that early return skips that Stop undoes is a ShowCursor(FALSE)
// (exe+0x21a75c), so the probe makes that call itself and the cursor's display count stays
// balanced (the game uses ShowCursor elsewhere). No game code is changed. The DX9 and tablet exes
// have the same player (exes.c); the DX9 exe loads the D3D9 driver anyway, so there it saves
// DirectShow, DirectDraw and the decoders only.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "probe.h"

typedef HRESULT(WINAPI *CoCreate_t)(REFCLSID, void *, DWORD, REFIID, LPVOID *);
static CoCreate_t g_real;
static const void *g_ret;          // the Play site's return address
static const volatile BYTE *g_skip;  // the game's SkipIntroVideo setting
static int g_always;               // SkipIntroPlayer=2: whatever that setting says
static volatile LONG g_refused;

// {E436EBB3-524F-11CE-9F53-0020AF0BA770}
static const BYTE kFilterGraph[16] = {0xB3, 0xEB, 0x36, 0xE4, 0x4F, 0x52, 0xCE, 0x11,
                                      0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};

// GameExe: introCreate (DX11 exe+0x21a3f4: push CLSID_FilterGraph; mov byte [esi+31h],1;
// call [CoCreateInstance] in Play), introSkipCopy (exe+0x135742: mov cl,[SkipIntroVideo];
// mov [eax+30h],cl in the owner), skipIntro (exe+0x8cf0c5).
const char *intro_check(BYTE *exe, const GameExe *g)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    if (!g->introCreate || !g->introSkipCopy || !g->skipIntro || g->skipIntro >= size) return "addresses";
    const BYTE *c = exe + g->introCreate - 9;
    BYTE *slot = exe_import_slot(exe, "CoCreateInstance");
    const BYTE *clsid = *(const BYTE *const *)(c + 1);
    if (c[0] != 0x68 || c[5] != 0xC6 || c[6] != 0x46 || c[7] != 0x31 || c[8] != 0x01 || c[9] != 0xFF || c[10] != 0x15 ||
        !slot || *(BYTE *const *)(c + 11) != slot)
        return "Play";
    if (clsid < exe || clsid + 16 > exe + size || memcmp(clsid, kFilterGraph, 16)) return "CLSID";
    const BYTE *s = exe + g->introSkipCopy;
    if (s[0] != 0x8A || s[1] != 0x0D || *(BYTE *const *)(s + 2) != exe + g->skipIntro || s[6] != 0x88 || s[7] != 0x48 ||
        s[8] != 0x30)
        return "SkipIntroVideo";
    return NULL;
}

static HRESULT WINAPI co_create(REFCLSID clsid, void *outer, DWORD ctx, REFIID iid, LPVOID *out)
{
    if (__builtin_return_address(0) == g_ret && (g_always || *g_skip) && clsid && !memcmp(clsid, kFilterGraph, 16)) {
        if (out) *out = NULL;
        if (InterlockedIncrement(&g_refused) == 1)
            log_event("intro: the intro's video player wasn't started (the game's SkipIntroVideo is %s; no DirectShow, "
                      "DirectDraw or Direct3D 9 driver loaded for it)", *g_skip ? "on" : "off");
        ShowCursor(FALSE);  // as the game's own failure path does (exe+0x21a75c); the player's Stop shows it again
        return REGDB_E_CLASSNOTREG;
    }
    return g_real(clsid, outer, ctx, iid, out);
}

void intro_init(const WCHAR *ini)
{
    int mode = GetPrivateProfileIntW(L"probe", L"SkipIntroPlayer", 2, ini);
    if (mode <= 0) return;
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(exe);
    const char *why = g ? intro_check(exe, g) : "not a known build of the game exe";
    void *volatile *slot = why ? NULL : (void *volatile *)exe_import_slot(exe, "CoCreateInstance");
    if (!why && !*slot) why = "import slot";
    if (why) {
        log_event("intro: the intro video player isn't the known code (%s); left alone", why);
        return;
    }
    g_real = (CoCreate_t)*slot;
    g_ret = exe + g->introCreate + 6;
    g_skip = exe + g->skipIntro;
    g_always = mode >= 2;
    DWORD old;
    if (!VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old)) return;
    InterlockedExchange((volatile LONG *)slot, (LONG)(ULONG_PTR)co_create);
    VirtualProtect((void *)slot, 4, old, &old);
    log_event("intro: the intro's video player won't be started%s (it would load DirectShow, DirectDraw and the "
              "Direct3D 9 driver for the rest of the session); no game code changed",
              g_always ? "" : " while the game's SkipIntroVideo is on");
}
